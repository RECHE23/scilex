/*!
 * \file fuzz_lexer.cpp
 * \brief libFuzzer robustness + correctness target for the SciLex lexer — the
 *        continuous, coverage-guided counterpart to `make fuzz-check`.
 *
 * Unlike REAL's fuzzer (no oracle — it only asserts the engine never crashes),
 * this target carries the property oracle (\ref reference.hpp): every input is
 * checked against an independent brute-force reference scanner, so a *wrong*
 * (not just crashing) tokenization is a finding. Three modes per input:
 *
 *   (a) every real example grammar (JSON, Python, C++, SQL, CSS, Lisp, math, XML,
 *       YAML) — all applicable invariants including indentation layout, and (for
 *       Python's f-strings, XML's content/tag, and YAML's block/flow) the per-mode
 *       dispatch over real multi-mode grammars;
 *   (b) a rule-set assembled from a palette of REAL patterns, seeded by the
 *       input itself — structural invariants only (no layout). This is where
 *       the first-byte dispatch meets rule orderings and bucket splits the
 *       nine fixed grammars never produce — exactly the dispatch's blind spots;
 *   (c) a multi-mode rule-set seeded by the input (push/pop/set, nesting,
 *       skip-transitions) — exercises the per-mode dispatch and the mode stack,
 *       which the mono-mode grammars (a)/(b) never reach.
 *
 * Sanitizers (ASan/UBSan) and libFuzzer's timeout cover no-crash and
 * termination; the oracle covers correctness. Build & run: `make fuzz`.
 */
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "reference.hpp"

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

  //! \brief A real example grammar: rules, lexer, and whether layout applies.
  struct grammar
  {
    const char              * name;
    std::vector<scilex::rule> (* rules)();
    scilex::lexer             (* lexer)();
    bool                      has_layout;
  };

  const grammar grammars[] {
    {"json", &scilex::examples::json::make_rules, &scilex::examples::json::make_lexer, false},
    {"python", &scilex::examples::python::make_rules, &scilex::examples::python::make_lexer, true},
    {"cpp", &scilex::examples::cpp::make_rules, &scilex::examples::cpp::make_lexer, false},
    {"sql", &scilex::examples::sql::make_rules, &scilex::examples::sql::make_lexer, false},
    {"css", &scilex::examples::css::make_rules, &scilex::examples::css::make_lexer, false},
    {"lisp", &scilex::examples::lisp::make_rules, &scilex::examples::lisp::make_lexer, false},
    {"math", &scilex::examples::math::make_rules, &scilex::examples::math::make_lexer, false},
    {"xml", &scilex::examples::xml::make_rules, &scilex::examples::xml::make_lexer, false},
    {"yaml", &scilex::examples::yaml::make_rules, &scilex::examples::yaml::make_lexer, true},
  };

  // A palette of valid REAL patterns for mode (b)'s random rule-sets. The mix
  // spans both dispatch paths: plain leading literals (bucketed by first byte —
  // '"', '/', '#', '@') and patterns with no fixed first byte (a leading
  // metaclass or a top-level alternation — the general list).
  const char* const palette[] {
    R"re(\s+)re",                                // whitespace (general: leading metachar)
    R"re([A-Za-z_][A-Za-z0-9_]*)re",             // identifier (general: leading class)
    R"re([0-9]+(\.[0-9]+)?)re",                  // number (general)
    R"re("(\\.|[^"\\])*")re",                    // string (bucket: '"')
    R"re(//.*)re",                               // line comment (bucket: '/')
    R"re(#[A-Za-z0-9]+)re",                      // hash run (bucket: '#')
    R"re(@[A-Za-z]+)re",                         // at-keyword (bucket: '@')
    R"re(<<=|>>=|::|->|==|!=|[-+*/%<>=&|^~])re", // operators (general: alternation)
    R"re([{}()\[\];,.])re",                      // punctuation (general)
    R"re(:=|<=|>=|<|>)re",                       // relational (general: alternation)
  };

  //! \brief FNV-1a hash of the input — a deterministic seed for the rule-set.
  std::uint64_t fnv1a(std::string_view bytes)
  {
    std::uint64_t hash {0xcbf29ce484222325ULL};
    for (const char byte : bytes) {
      hash ^= static_cast<unsigned char>(byte);
      hash *= 0x100000001b3ULL;
    }
    return hash;
  }

  //! \brief Assembles a rule-set from \p seed: a rotated subset of the palette,
  //!        each rule independently included and marked skip by seed bits, plus an
  //!        optional case-insensitive keyword rule (exercises the flags -> general
  //!        dispatch path). Guaranteed non-empty and usable.
  std::vector<scilex::rule> rules_from_seed(std::uint64_t seed)
  {
    const std::size_t         count {sizeof(palette) / sizeof(palette[0])};
    const std::size_t         start {static_cast<std::size_t>(seed % count)};
    std::vector<scilex::rule> rules;
    int                       kind  {1};

    if ((seed & (1ULL << 60)) != 0) { // a flagged rule has no fixed first byte
      rules.push_back({kind++, real::regex("select|from|where", real::flags::icase), false});
    }
    for (std::size_t k {0}; k < count; ++k) {
      const std::size_t index {(start + k) % count};
      if (((seed >> k) & 1ULL) != 0) {
        const bool skip {((seed >> (k + 16)) & 1ULL) != 0};
        rules.push_back({kind++, real::regex(palette[index]), skip});
      }
    }
    if (rules.size() < 2) { // guarantee something can match most bytes
      rules.push_back({900, real::regex(R"re(\s+)re"), true});
      rules.push_back({901, real::regex(R"re(.)re"), false});
    }
    return rules;
  }

  // --- mode (c): multi-mode rule-sets ----------------------------------------
  scilex::mode_action push_act(const char* mode)
  {
    return {.operation = scilex::mode_action::op::push, .target = mode};
  }

  scilex::mode_action set_act(const char* mode)
  {
    return {.operation = scilex::mode_action::op::set, .target = mode};
  }

  scilex::mode_action pop_act()
  {
    return {.operation = scilex::mode_action::op::pop};
  }

  scilex::rule mode_in(int                      kind,
                       const char             * pattern,
                       std::vector<std::string> modes,
                       bool                     skip)
  {
    scilex::rule rule {.kind = kind, .pattern = real::regex(pattern), .skip = skip};
    rule.in_mode = std::move(modes);
    return rule;
  }

  scilex::rule with_act(scilex::rule               rule,
                        const scilex::mode_action& action)
  {
    rule.action = action;
    return rule;
  }

  // A self-contained mode feature: an opener (in default) entering `mode`, a body
  // rule in `mode`, and a closer leaving it. Every feature carries its target
  // mode's rules, so *any* subset assembles into a constructible grammar.
  struct mode_feature
  {
    const char* open;
    const char* mode;
    const char* body;
    const char* close;
    bool        use_set; // set/set (no nesting, no unterminated) vs push/pop
  };

  const mode_feature features[] {
    {R"re(")re",  "str", R"re([^"]+)re",  R"re(")re",  false}, // "..."
    {R"re(\()re", "grp", R"re([a-z]+)re", R"re(\))re", false}, // (...) — nests
    {R"re(#)re",  "cmt", R"re([^\n]+)re", R"re(\n)re", true},  // #...\n — set in place
    {R"re(<<)re", "raw", R"re([^>]+)re",  R"re(>>)re", false}, // <<...>>
  };

  //! \brief Assembles a multi-mode grammar from \p seed (push/pop/set, nesting),
  //!        constructible by design (no transition targets an empty mode).
  std::vector<scilex::rule> multi_mode_rules_from_seed(std::uint64_t seed)
  {
    const std::size_t         count {sizeof(features) / sizeof(features[0])};
    std::vector<scilex::rule> rules;
    int                       kind  {1};
    rules.push_back({kind++, real::regex(R"re([a-z]+)re"), false}); // default ident
    rules.push_back({kind++, real::regex(R"re(\s+)re"), ((seed >> 1) & 1ULL) != 0});

    bool any {false};
    for (std::size_t f {0}; f < count; ++f) {
      if (((seed >> f) & 1ULL) == 0) {
        continue; // this feature is off for this seed
      }
      any = true;
      const mode_feature& feat       {features[f]};
      const bool          delim_skip {((seed >> (f + 8)) & 1ULL) != 0};
      if (feat.use_set) {
        rules.push_back(with_act(mode_in(kind++, feat.open, {"default"}, delim_skip), set_act(feat.mode)));
        rules.push_back(mode_in(kind++, feat.body, {feat.mode}, false));
        rules.push_back(with_act(mode_in(kind++, feat.close, {feat.mode}, delim_skip), set_act("default")));
      }
      else {
        rules.push_back(with_act(mode_in(kind++, feat.open, {"default", feat.mode}, delim_skip), push_act(feat.mode)));
        rules.push_back(mode_in(kind++, feat.body, {feat.mode}, false));
        rules.push_back(with_act(mode_in(kind++, feat.close, {feat.mode}, delim_skip), pop_act()));
      }
    }
    if (!any) { // always exercise at least one feature
      const mode_feature& feat {features[seed % count]};
      rules.push_back(with_act(mode_in(kind++, feat.open, {"default", feat.mode}, false), push_act(feat.mode)));
      rules.push_back(mode_in(kind++, feat.body, {feat.mode}, false));
      rules.push_back(with_act(mode_in(kind++, feat.close, {feat.mode}, false), pop_act()));
    }
    rules.push_back({kind++, real::regex(R"re(.)re"), false}); // default catch-all
    return rules;
  }

  //! \brief Runs the oracle against \p lex and the recovery oracle against \p token_lex; on a
  //!        violation, reports and aborts (a libFuzzer find).
  void run_or_die(const char*                      label,
                  const std::vector<scilex::rule>& rules,
                  const scilex::lexer&             lex,
                  const scilex::lexer&             token_lex,
                  std::string_view                 input,
                  bool                             has_layout)
  {
    const scilex::fuzz::result outcome {scilex::fuzz::check(rules, lex, input, has_layout)};
    if (!outcome.ok) {
      std::fprintf(stderr, "oracle violation [%s]: %s\n", label, outcome.invariant);
      std::abort();
    }
    // Error recovery: the token lexer must equal the recovery reference on the same input (the
    // continuous counterpart of fuzz-check's token pass).
    const scilex::fuzz::result recovered {scilex::fuzz::check_recover(rules, token_lex, input)};
    if (!recovered.ok) {
      std::fprintf(stderr, "oracle violation [%s token]: %s\n", label, recovered.invariant);
      std::abort();
    }
  }

  //! \brief The recovery lexer the oracle drives: dfa_modes = {"default"} exercises both scan paths'
  //!        ERROR handling.
  scilex::lexer token_lexer(const std::vector<scilex::rule>& rules)
  {
    return scilex::lexer {rules, {}, {"default"}, scilex::error_policy::token};
  }

  //! \brief A fixed grammar, built ONCE. A lexer is immutable and shareable (scan state lives in the
  //!        scan), so rebuilding the nine grammars -- each with a DFA-accelerated recovery lexer whose
  //!        construction decides every rule -- for every input only spent the budget on construction:
  //!        the target ran at a fraction of an execution per second.
  struct prepared_grammar
  {
    const grammar*            source;
    std::vector<scilex::rule> rules;
    scilex::lexer             lex;
    scilex::lexer             token_lex;
  };

  const std::vector<prepared_grammar>& prepared_grammars()
  {
    static const std::vector<prepared_grammar> all {[] {
                                                      std::vector<prepared_grammar> built;
                                                      for (const grammar& gram : grammars) {
                                                        std::vector<scilex::rule> rules {gram.rules()};
                                                        scilex::lexer             lex {gram.lexer()};
                                                        scilex::lexer             token_lex {token_lexer(rules)};
                                                        built.push_back({&gram, std::move(rules), std::move(lex),
                                                                         std::move(token_lex)});
                                                      }
                                                      return built;
                                                    }()};
    return all;
  }

  //! \brief A seeded rule-set with its two lexers, built once per slot. Building a lexer decides every
  //!        rule and builds its DFAs, which cost more than lexing and checking an 8 KiB input: with four
  //!        lexers built per input the target ran at under one execution per second. The input picks one
  //!        of \ref seeded_slots rule-sets per mode; the space of rule-sets itself is swept exhaustively
  //!        by `make exhaustive-lex`, so the fuzzer spends its time on inputs.
  struct seeded_grammar
  {
    std::vector<scilex::rule>    rules;
    std::optional<scilex::lexer> lex;       //!< Empty when the assembly is unconstructible (mode c).
    std::optional<scilex::lexer> token_lex;
  };

  constexpr std::uint64_t seeded_slots {256};

  //! \brief How many of the fixed grammars one input is checked against (see LLVMFuzzerTestOneInput).
  constexpr std::size_t grammars_per_input {3};

  //! \brief The slot's rule-set from \p assemble and its lexers, built on first use.
  //! \param[in] cache    One mode's slots.
  //! \param[in] slot     The slot, below \ref seeded_slots.
  //! \param[in] assemble The mode's rule-set generator, given the slot's own seed.
  //! \return The slot's grammar.
  const seeded_grammar& seeded_slot(std::vector<std::unique_ptr<seeded_grammar>>& cache,
                                    std::uint64_t                                 slot,
                                    std::vector<scilex::rule>                   (*assemble)(std::uint64_t))
  {
    std::unique_ptr<seeded_grammar>& entry {cache[slot]};
    if (!entry) {
      entry        = std::make_unique<seeded_grammar>();
      entry->rules = assemble(fnv1a(std::to_string(slot)));
      try {
        entry->lex.emplace(entry->rules); // ctor copies; rules stays for the reference
        entry->token_lex.emplace(token_lexer(entry->rules));
      }
      catch (const std::invalid_argument&) {
        entry->lex.reset(); // an unconstructible assembly: skipped, not a finding
        entry->token_lex.reset();
      }
    }
    return *entry;
  }
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t         size)
{
  const std::string_view input {reinterpret_cast<const char*>(data), size};

  // Mode (a): real grammars, all applicable invariants (incl. layout). The independent reference costs a
  // match call per rule per position, so checking all nine grammars on every input left the target near
  // one execution per second; each input checks grammars_per_input of them, in a rotation its hash picks,
  // and `make fuzz-check` still runs all nine over its fixed inputs on every gate.
  const std::uint64_t                         hash  {fnv1a(input)};
  const std::vector<prepared_grammar>&        all   {prepared_grammars()};
  const std::size_t                           first {static_cast<std::size_t>(hash % all.size())};
  for (std::size_t k {0}; k < std::min(grammars_per_input, all.size()); ++k) {
    const prepared_grammar& gram {all[(first + k) % all.size()]};
    run_or_die(gram.source->name, gram.rules, gram.lex, gram.token_lex, input, gram.source->has_layout);
  }

  // Mode (b): a rule-set picked by the input (structural, no layout).
  static std::vector<std::unique_ptr<seeded_grammar>> random_slots(seeded_slots);
  const seeded_grammar&                               random {seeded_slot(random_slots, hash % seeded_slots, &rules_from_seed)};
  if (random.lex) {
    run_or_die("random", random.rules, *random.lex, *random.token_lex, input, false);
  }

  // Mode (c): a multi-mode rule-set, picked distinctly. Valid by construction, but the build is
  // guarded so that a future palette change can never turn a bad assembly into a crash.
  static std::vector<std::unique_ptr<seeded_grammar>> multimode_slots(seeded_slots);
  const seeded_grammar&                               multimode {seeded_slot(multimode_slots, (hash >> 32U) % seeded_slots,
                                                                             &multi_mode_rules_from_seed)};
  if (multimode.lex) {
    run_or_die("multimode", multimode.rules, *multimode.lex, *multimode.token_lex, input, false);
  }

  return 0;
}
