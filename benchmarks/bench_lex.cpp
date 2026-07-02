/*!
 * \file bench_lex.cpp
 * \brief Per-grammar C++ throughput baseline for the SciLex engine (raw collector).
 *
 * Complements \c benchmarks/bench.py (which times the Python binding against
 * \c re): this measures the pure C++ lexer directly, on each of the nine
 * example grammars, over realistic steady-state inputs built by scaling the
 * grammar's own sample. It collects four sections:
 *
 *   - grammar       — \c tokenize() (eager) vs \c scan() (lazy), per grammar;
 *   - linearity     — one grammar over growing sizes (flat throughput => linear);
 *   - mode-overhead — the modal Python grammar vs a mono-mode baseline;
 *   - dfa-modes     — the per-mode DFA fast path vs Pike (build cost + steady state).
 *
 * This program ONLY collects raw per-call samples (seconds) and emits the canonical
 * sciforge.bench JSON (emit_case/emit_run); benchmarks/bench_lex.py derives MB/s, the
 * minimum, the speed-up and the build cost. The timing primitives and the JSON emitter live
 * in SciForge's shared C++ collector (include/sciforge/bench.hpp). Each input is scaled once,
 * the lexer is built once, the thunk is warmed up, and the thunk's work quantity is observed
 * (do_not_optimize, inside collect) so nothing is optimized away. Informational only — never
 * gated; see `make bench-lex` / `make bench`.
 */
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sciforge/bench.hpp>

#include "cpp.hpp"
#include "css.hpp"
#include "json.hpp"
#include "lisp.hpp"
#include "math.hpp"
#include "python.hpp"
#include "sql.hpp"
#include "xml.hpp"
#include "yaml.hpp"

namespace {

  constexpr std::size_t target_bytes {256 * 1024}; //!< Steady-state input size.

  std::vector<std::string> g_cases;                //!< The emitted canonical Cases, in order.

  using field   = std::pair<std::string, std::string>;
  using domain  = std::vector<field>;

  //! \brief A domain field whose value is a JSON number (bytes / tokens).
  field num(const char* key,
            std::size_t value)
  {
    return {key, sciforge::bench::json_number(static_cast<double>(value))};
  }

  //! \brief A domain field whose value is a JSON string (section / grammar / path / variant).
  field str(const char     * key,
            std::string_view value)
  {
    return {key, sciforge::bench::json_string(std::string(value))};
  }

  //! \brief Repeats \p sample (newline-separated) until at least \p target bytes.
  std::string scale(std::string_view sample,
                    std::size_t      target)
  {
    std::string out;
    out.reserve(target + sample.size() + 1);
    while (out.size() < target) {
      out.append(sample);
      out.push_back('\n'); // separate copies so no token merges across the join
    }
    return out;
  }

  //! \brief 3 warmups (untimed), then 9 raw per-call samples (seconds) for \p run, appended as
  //!        one Case with \p domain. samples=9/inner=1 mirrors the old min-of-9 best; the
  //!        minimum is taken by the reporter. The thunk's work is observed inside collect.
  template <class Run>
  void measure(const std::string& name,
               Run&&              run,
               const domain&      fields)
  {
    for (int i {0}; i < 3; ++i) {
      run();
    }
    const std::vector<double> samples {sciforge::bench::collect(run, 9, 1)};
    g_cases.push_back(sciforge::bench::emit_case(name, "s", samples, fields));
  }

  //! \brief Emits the eager + lazy cases for one grammar (the Pike engine, built from
  //!        \p make_rules — apples-to-apples across all nine grammars).
  void grammar_case(const char                 * name,
                    std::vector<scilex::rule> (* make_rules)(),
                    std::string_view             sample)
  {
    const std::string   source {scale(sample, target_bytes)};
    const scilex::lexer lex    {make_rules()};
    const std::size_t   tokens {lex.tokenize(source).size()};
    const auto          base   {[&](const char* path) {
                                  return domain {str("section", "grammar"), str("grammar", name),
                                                 str("path", path), num("bytes", source.size()),
                                                 num("tokens", tokens)};
                                }};
    measure(std::string(name) + " eager",
            [&] { return lex.tokenize(source).size(); }, base("eager"));
    measure(std::string(name) + " lazy", [&] {
              std::size_t consumed {0};
              for (const scilex::token& tok : lex.scan(source)) {
                consumed += tok.lexeme.size();
              }
              return consumed;
            }, base("lazy"));
  }

  // --- Adversarial corpus for the failure-cost baseline (deterministic, versioned here) ---------
  // Inputs that a lexer's rules reject, forcing the recover-and-resync path. Each is built the same
  // way on every run so the numbers are comparable across revisions.

  //! \brief `n` bytes cycling through all 256 values — binary junk no text grammar tokenizes.
  std::string binary_block(std::size_t n)
  {
    std::string out;
    out.reserve(n);
    for (std::size_t i {0}; i < n; ++i) {
      out.push_back(static_cast<char>((i * 37U + 17U) & 0xFFU));
    }
    return out;
  }

  //! \brief A long run of invalid UTF-8 lead bytes (0xFF/0xFE), never a valid code point.
  std::string invalid_utf8_run(std::size_t n)
  {
    return std::string(n, '\xFF');
  }

  //! \brief One quote then a long unterminated body (a string that never closes).
  std::string unclosed_quote(std::size_t n)
  {
    std::string out(1, '"');
    out.append(n - 1, 'x');
    return out;
  }

  //! \brief Punctuation outside every grammar's alphabet, densely — one failure per byte.
  std::string parasitic_delims(std::size_t n)
  {
    static constexpr char marks[] {'@', '#', '$', '%', '^', '&', '`', '~'};
    std::string           out;
    out.reserve(n);
    for (std::size_t i {0}; i < n; ++i) {
      out.push_back(marks[i % sizeof(marks)]);
    }
    return out;
  }

  //! \brief The recover-and-resync loop, over the public API: tokenize from a cursor; on a lexical
  //!        error, emit the good prefix, step past the offending byte, and resume. Returns tokens
  //!        consumed and, via \p failures, how many positions were rejected (one throw each). The
  //!        string_view substr is O(1). NB: each recovery is a fresh \c tokenize call, so this measures
  //!        the NAIVE resync -- it carries tokenize()'s fixed per-call setup at every recovery point.
  //!        An in-lexer resync would reuse a cursor and avoid that; this loop is thus an upper bound on
  //!        the per-position cost, and its throw overhead is isolated below.
  std::size_t resync(const scilex::lexer& lex,
                     std::string_view     source,
                     std::size_t&         failures)
  {
    std::size_t pos      {0};
    std::size_t consumed {0};
    failures = 0;
    while (pos < source.size()) {
      try {
        consumed += lex.tokenize(source.substr(pos)).size();
        break; // reached the end cleanly
      }
      catch (const scilex::lex_error& error) {
        ++failures;
        pos += error.where().offset + 1; // emit the good prefix, then step past the bad byte
      }
    }
    return consumed;
  }

  //! \brief The cost of \p n throw+catch pairs ALONE — the exception mechanism the future
  //!        incremental lexer will NOT pay per byte (it recovers without throwing). Subtracted from
  //!        the resync cost to isolate the net per-position probe cost.
  std::size_t throw_overhead(std::size_t n)
  {
    std::size_t sink {0};
    for (std::size_t i {0}; i < n; ++i) {
      try {
        throw scilex::lex_error("probe", scilex::position {i, 1, 1});
      }
      catch (const scilex::lex_error& error) {
        sink += error.where().offset;
      }
    }
    return sink;
  }
} // namespace

int main()
{
  // --- grammar: eager vs lazy across the nine grammars (Pike engine). ---
  grammar_case("json", &scilex::examples::json::make_rules, scilex::examples::json::sample);
  grammar_case("python", &scilex::examples::python::make_rules, scilex::examples::python::sample);
  grammar_case("cpp", &scilex::examples::cpp::make_rules, scilex::examples::cpp::sample);
  grammar_case("sql", &scilex::examples::sql::make_rules, scilex::examples::sql::sample);
  grammar_case("css", &scilex::examples::css::make_rules, scilex::examples::css::sample);
  grammar_case("lisp", &scilex::examples::lisp::make_rules, scilex::examples::lisp::sample);
  grammar_case("math", &scilex::examples::math::make_rules, scilex::examples::math::sample);
  grammar_case("xml", &scilex::examples::xml::make_rules, scilex::examples::xml::sample);
  grammar_case("yaml", &scilex::examples::yaml::make_rules, scilex::examples::yaml::sample);

  // --- linearity: cpp grammar over growing sizes (flat MB/s => linear time). ---
  const scilex::lexer clex {scilex::examples::cpp::make_lexer()};
  for (const std::size_t kib : {std::size_t {64}, std::size_t {128}, std::size_t {256}, std::size_t {512}}) {
    const std::string source {scale(scilex::examples::cpp::sample, kib * 1024)};
    measure("linearity " + std::to_string(source.size() / 1024) + "KiB",
            [&] { return clex.tokenize(source).size(); },
            domain {str("section", "linearity"), num("bytes", source.size())});
  }

  // --- mode overhead: the modal Python grammar vs a mono-mode baseline on the same sample. ---
  namespace py = scilex::examples::python;
  const std::string pysrc {scale(py::sample, target_bytes)};
  {
    const scilex::lexer modal {py::make_lexer()};
    const std::size_t   toks  {modal.tokenize(pysrc).size()};
    measure("modal", [&] { return modal.tokenize(pysrc).size(); },
            domain {str("section", "mode-overhead"), str("variant", "modal"),
                    num("bytes", pysrc.size()), num("tokens", toks)});
  }
  {
    std::vector<scilex::rule> mono;
    for (scilex::rule& candidate : py::make_rules()) {
      const int kind {candidate.kind};
      if (kind == py::fstr_open || kind == py::fstr_text || kind == py::fstr_close
          || kind == py::interp_open || kind == py::interp_close) {
        continue; // drop the f-string mode rules (f-strings become NAME + string)
      }
      candidate.in_mode.clear();
      candidate.action.reset();
      mono.push_back(std::move(candidate));
    }
    const scilex::lexer base {std::move(mono)};
    const std::size_t   toks {base.tokenize(pysrc).size()};
    measure("mono-mode", [&] { return base.tokenize(pysrc).size(); },
            domain {str("section", "mode-overhead"), str("variant", "mono-mode"),
                    num("bytes", pysrc.size()), num("tokens", toks)});
  }

  // --- dfa-modes: the per-mode DFA fast path vs Pike (build cost + steady state). ---
  struct dfa_bench
  {
    const char              * name;
    std::vector<scilex::rule> (* rules)();
    std::string_view          sample;
  };
  const dfa_bench dfa_grammars[] {
    // ASCII-pinned grammars: their default mode is DFA-representable, so it accelerates.
    {.name = "json", .rules = &scilex::examples::json::make_rules, .sample = scilex::examples::json::sample},
    {.name = "sql", .rules = &scilex::examples::sql::make_rules, .sample = scilex::examples::sql::sample},
    {.name = "css", .rules = &scilex::examples::css::make_rules, .sample = scilex::examples::css::sample},
    {.name = "lisp", .rules = &scilex::examples::lisp::make_rules, .sample = scilex::examples::lisp::sample},
    {.name = "math", .rules = &scilex::examples::math::make_rules, .sample = scilex::examples::math::sample},
    // Text-mode grammars: their \s+/\w are code-point predicates the DFA cannot represent, so the
    // default mode is rejected and stays on Pike — dfa_modes_active is empty (active=false).
    {.name = "xml", .rules = &scilex::examples::xml::make_rules, .sample = scilex::examples::xml::sample},
    {.name = "yaml", .rules = &scilex::examples::yaml::make_rules, .sample = scilex::examples::yaml::sample},
  };
  for (const dfa_bench& grammar : dfa_grammars) {
    const std::string   source      {scale(grammar.sample, target_bytes)};
    const scilex::lexer pike        {grammar.rules()};
    const scilex::lexer dfa         {grammar.rules(), {}, {"default"}};
    const bool          accelerated {!dfa.dfa_modes_active().empty()};
    const std::size_t   tokens      {pike.tokenize(source).size()};
    const auto          base        {[&](const char* path) {
                                       return domain {str("section", "dfa-modes"), str("grammar", grammar.name),
                                                      str("path", path), num("bytes", source.size()),
                                                      num("tokens", tokens),
                                                      field {"active", accelerated ? "true" : "false"}};
                                     }};
    measure(std::string(grammar.name) + " build", [&] {
              const scilex::lexer once {grammar.rules(), {}, {"default"}};
              return once.dfa_modes_active().size();
            }, base("build"));
    measure(std::string(grammar.name) + " pike", [&] { return pike.tokenize(source).size(); }, base("pike"));
    measure(std::string(grammar.name) + " dfa", [&] { return dfa.tokenize(source).size(); }, base("dfa"));
  }
  // 0-regression control: python's default falls back to Pike, so DFA-on and DFA-off tokenize
  // at the same rate — the per_mode_dfa_ check is free.
  {
    const std::string   source {scale(py::sample, target_bytes)};
    const scilex::lexer off    {py::make_rules()};
    const scilex::lexer on     {py::make_rules(), {}, {"default"}}; // default rejected (lazy) → Pike
    const bool          active {!on.dfa_modes_active().empty()};
    const std::size_t   tokens {off.tokenize(source).size()};
    const auto          base   {[&](const char* path) {
                                  return domain {str("section", "dfa-modes"), str("grammar", "py*"),
                                                 str("path", path), num("bytes", source.size()),
                                                 num("tokens", tokens), field {"active", active ? "true" : "false"}};
                                }};
    measure("py* off", [&] { return off.tokenize(source).size(); }, base("off"));
    measure("py* on", [&] { return on.tokenize(source).size(); }, base("on"));
  }

  // --- failure-cost: the per-invalid-byte cost of the recover-and-resync loop. -----------------
  // On adversarial input (bytes no rule matches) the future incremental lexer must skip the bad byte
  // and resume. We run that loop here on two engine paths -- an ASCII grammar whose default mode is
  // DFA-accelerated (fast rejection), and a text-mode grammar on Pike (the worst case) -- and isolate
  // the exception overhead so the NET probe cost is comparable to what M1 (which will not throw per
  // byte) would pay.
  {
    struct corpus_case
    {
      const char* name;
      std::string data;
    };
    const std::size_t              corpus_bytes {32 * 1024};
    const std::vector<corpus_case> corpora      {
      {"binary", binary_block(corpus_bytes)},
      {"invalid-utf8", invalid_utf8_run(corpus_bytes)},
      {"unclosed-quote", unclosed_quote(corpus_bytes)},
      {"parasitic-delims", parasitic_delims(corpus_bytes)},
    };
    struct engine_path
    {
      const char              * name; // "dfa" (ASCII, accelerated) or "pike" (text-mode)
      const char              * grammar;
      std::vector<scilex::rule> (* rules)();
      bool                      accelerate;
    };
    const engine_path paths[] {
      {.name = "dfa", .grammar = "json", .rules = &scilex::examples::json::make_rules, .accelerate = true},
      {.name = "pike", .grammar = "xml", .rules = &scilex::examples::xml::make_rules, .accelerate = false},
    };

    for (const engine_path& path : paths) {
      const scilex::lexer lex    {path.accelerate ? scilex::lexer {path.rules(), {}, {"default"}}
                                               : scilex::lexer {path.rules()}};
      const bool          active {!lex.dfa_modes_active().empty()};
      for (const corpus_case& corpus : corpora) {
        std::size_t failures {0};
        resync(lex, corpus.data, failures); // count the rejected positions once, outside timing
        measure(std::string(path.name) + " " + corpus.name,
                [&] { std::size_t f {0}; return resync(lex, corpus.data, f); },
                domain {str("section", "failure-cost"), str("path", path.name), str("grammar", path.grammar),
                        str("corpus", corpus.name), num("bytes", corpus.data.size()),
                        num("failures", failures), field {"active", active ? "true" : "false"}});
      }
    }

    // The exception mechanism alone (throw+catch), to subtract from the resync cost -- M1 recovers
    // without throwing, so this overhead is not part of its per-byte cost.
    const std::size_t throw_n {corpus_bytes};
    measure("throw-overhead",
            [&] { return throw_overhead(throw_n); },
            domain {str("section", "failure-cost"), str("path", "throw-isolated"),
                    num("throws", throw_n)});

    // Non-fail-fast rule: `[^!]*!` consumes a maximal non-'!' run before requiring '!', so on input
    // with no '!' it scans to the end and fails -- every resync position re-scans O(remaining). This
    // is the O(rest)-per-position characteristic M1 documents (and a first-byte prefilter mitigates).
    {
      const std::size_t         nff_bytes {8 * 1024}; // smaller: this path is quadratic by design
      const std::string         run(nff_bytes, 'x');  // no '!': the rule never completes
      std::vector<scilex::rule> nff;
      nff.push_back(scilex::rule {.kind = 0, .pattern = real::regex("[^!]*!", real::flags::ascii), .skip = false});
      const scilex::lexer nff_lex  {std::move(nff)};
      std::size_t         failures {0};
      resync(nff_lex, run, failures);
      measure("non-fail-fast",
              [&] { std::size_t f {0}; return resync(nff_lex, run, f); },
              domain {str("section", "failure-cost"), str("path", "non-fail-fast"), str("grammar", "greedy-scan"),
                      str("corpus", "no-terminator"), num("bytes", run.size()), num("failures", failures)});
    }
  }

  const std::string meta {sciforge::bench::json_object(
                            {{"bench", sciforge::bench::json_string("lex")},
                              {"compiler", sciforge::bench::json_string(__VERSION__)}})};
  std::printf("%s\n", sciforge::bench::emit_run(meta, g_cases).c_str());
  return 0;
}
