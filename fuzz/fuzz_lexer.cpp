/*!
 * \file fuzz_lexer.cpp
 * \brief libFuzzer robustness + correctness target for the SciLex lexer — the
 *        continuous, coverage-guided counterpart to `make fuzz-check`.
 *
 * Unlike REAL's fuzzer (no oracle — it only asserts the engine never crashes),
 * this target carries the property oracle (\ref reference.hpp): every input is
 * checked against an independent brute-force reference scanner, so a *wrong*
 * (not just crashing) tokenization is a finding. Two modes per input:
 *
 *   (a) every real example grammar (JSON, Python, C++, SQL, CSS, Lisp, math) —
 *       all applicable invariants including indentation layout;
 *   (b) a rule-set assembled from a palette of REAL patterns, seeded by the
 *       input itself — structural invariants only (no layout). This is where
 *       the first-byte dispatch meets rule orderings and bucket splits the
 *       seven fixed grammars never produce — exactly the dispatch's blind spots.
 *
 * Sanitizers (ASan/UBSan) and libFuzzer's timeout cover no-crash and
 * termination; the oracle covers correctness. Build & run: `make fuzz`.
 */
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

  //! \brief Runs the oracle; on a violation, reports and aborts (a libFuzzer find).
  void run_or_die(const char*                      label,
                  const std::vector<scilex::rule>& rules,
                  const scilex::lexer&             lex,
                  std::string_view                 input,
                  bool                             has_layout)
  {
    const scilex::fuzz::result outcome {scilex::fuzz::check(rules, lex, input, has_layout)};
    if (!outcome.ok) {
      std::fprintf(stderr, "oracle violation [%s]: %s\n", label, outcome.invariant);
      std::abort();
    }
  }
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t         size)
{
  const std::string_view input {reinterpret_cast<const char*>(data), size};

  // Mode (a): every real grammar, all applicable invariants (incl. layout).
  for (const grammar& gram : grammars) {
    const std::vector<scilex::rule> rules {gram.rules()};
    const scilex::lexer             lex   {gram.lexer()};
    run_or_die(gram.name, rules, lex, input, gram.has_layout);
  }

  // Mode (b): a rule-set seeded by the input itself (structural, no layout).
  const std::vector<scilex::rule> rules {rules_from_seed(fnv1a(input))};
  const scilex::lexer             lex   {rules}; // ctor copies; rules stays for the reference
  run_or_die("random", rules, lex, input, false);

  return 0;
}
