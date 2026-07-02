/*!
 * \file oracle_check.cpp
 * \brief Deterministic, seeded gate over the fuzzing oracle (\ref reference.hpp).
 *
 * Runs every property invariant for each of the nine example grammars across a
 * fixed adversarial input set — the sample, every prefix of it (truncations cut
 * tokens mid-stream), and shared hostile strings (empty, NUL/control/high bytes,
 * invalid UTF-8, unmatched delimiters, an unterminated string, long runs). This
 * is the bounded, repeatable half of the property-based oracle — wired into `make fuzz-check` /
 * `full-local-gate`; the continuous explorer is `make fuzz`. On a violation it
 * prints the grammar, the offending input, and the invariant, and exits 1.
 */
#include <iostream>
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

  //! \brief A registered grammar: its rule list, lexer, sample, and layout flag.
  struct grammar
  {
    std::string_view          name;
    std::vector<scilex::rule> (* rules)();
    scilex::lexer             (* lexer)();
    std::string_view          sample;
    bool                      has_layout;
  };

  const std::vector<grammar> grammars {
    {"json", &scilex::examples::json::make_rules, &scilex::examples::json::make_lexer,
     scilex::examples::json::sample, false},
    {"python", &scilex::examples::python::make_rules, &scilex::examples::python::make_lexer,
     scilex::examples::python::sample, true},
    {"cpp", &scilex::examples::cpp::make_rules, &scilex::examples::cpp::make_lexer,
     scilex::examples::cpp::sample, false},
    {"sql", &scilex::examples::sql::make_rules, &scilex::examples::sql::make_lexer,
     scilex::examples::sql::sample, false},
    {"css", &scilex::examples::css::make_rules, &scilex::examples::css::make_lexer,
     scilex::examples::css::sample, false},
    {"lisp", &scilex::examples::lisp::make_rules, &scilex::examples::lisp::make_lexer,
     scilex::examples::lisp::sample, false},
    {"math", &scilex::examples::math::make_rules, &scilex::examples::math::make_lexer,
     scilex::examples::math::sample, false},
    {"xml", &scilex::examples::xml::make_rules, &scilex::examples::xml::make_lexer,
     scilex::examples::xml::sample, false},
    {"yaml", &scilex::examples::yaml::make_rules, &scilex::examples::yaml::make_lexer,
     scilex::examples::yaml::sample, true},
  };

  // A fixed, hostile input set shared across grammars (deterministic).
  const std::vector<std::string> adversarial {
    std::string {},                                  // empty
    std::string {'\0', '\x01', '\x02', '\x7f'},      // NUL + control bytes
    std::string {'\xff', '\xfe', '\xfd'},            // invalid UTF-8 lead bytes
    std::string {'\xc3', '\x28'},                    // invalid UTF-8 continuation
    std::string {'"', 'u', 'n', 't', 'e', 'r', 'm'}, // unterminated string-ish
    std::string {'}', '{', ']', '[', ')', '('},      // unmatched delimiters
    std::string(300, 'a'),                           // a long identifier-ish run
    std::string {' ', '\t', '\n', '\n', ' '},        // whitespace / newlines only
    std::string {'#', '!', '@', '$', '\\'},          // odd punctuation
  };

  // The nominal corpus for error recovery (error_policy::token): dense adversarial inputs no grammar
  // fully lexes, so the recover-and-resync path is the common case. Deterministic; kept modest in size
  // so the gate stays fast (recovery is exercised for correctness here, not throughput).
  std::string cycle_bytes(std::size_t n)
  {
    std::string out;
    out.reserve(n);
    for (std::size_t i {0}; i < n; ++i) {
      out.push_back(static_cast<char>((i * 37U + 17U) & 0xFFU));
    }
    return out;
  }

  std::string parasitic(std::size_t n)
  {
    static constexpr char marks[] {'@', '#', '$', '%', '^', '&', '`', '~'};
    std::string           out;
    out.reserve(n);
    for (std::size_t i {0}; i < n; ++i) {
      out.push_back(marks[i % sizeof(marks)]);
    }
    return out;
  }

  const std::vector<std::string> token_corpus {
    cycle_bytes(1024),                                      // a binary block
    std::string(1024, '\xff'),                              // a long invalid-UTF-8 run
    "\"" + std::string(1023, 'x'),                          // an unclosed quote and its body
    parasitic(1024),                                        // parasitic delimiters
    "12 @#$ 34 \xff\xff 56 }{ 78",                          // valid tokens interleaved with noise
    std::string {'\0', '\x01', 'a', 'b', '\x7f', '1', '2'}, // control bytes among real tokens
  };

  // Escapes non-printable bytes for a readable failure report.
  std::string preview(std::string_view input)
  {
    std::string out;
    for (std::size_t i {0}; i < input.size() && i < 60; ++i) {
      const unsigned char byte {static_cast<unsigned char>(input[i])};
      if (byte >= 0x20 && byte < 0x7f) {
        out.push_back(static_cast<char>(byte));
      }
      else {
        out += "\\x";
        const char* const hex {"0123456789abcdef"};
        out.push_back(hex[byte >> 4]);
        out.push_back(hex[byte & 0xf]);
      }
    }
    return out;
  }

  // --- multi-mode grammars ---------------------------------------------------
  // Crafted grammars that drive push/pop/set, nesting, skip-transitions and the
  // three positioned errors, checked against the lexer. The reference shares
  // apply_transition (so the transition cannot diverge); any disagreement here is
  // a real per-mode dispatch bug.
  scilex::mode_action act_push(const char* mode)
  {
    return {.operation = scilex::mode_action::op::push, .target = mode};
  }

  scilex::mode_action act_set(const char* mode)
  {
    return {.operation = scilex::mode_action::op::set, .target = mode};
  }

  scilex::mode_action act_pop()
  {
    return {.operation = scilex::mode_action::op::pop};
  }

  scilex::rule in_mode_rule(int                      kind,
                            const char             * pattern,
                            std::vector<std::string> modes,
                            bool                     skip = false)
  {
    scilex::rule rule {.kind = kind, .pattern = real::regex(pattern), .skip = skip};
    rule.in_mode = std::move(modes);
    return rule;
  }

  scilex::rule acting(scilex::rule               rule,
                      const scilex::mode_action& action)
  {
    rule.action = action;
    return rule;
  }

  // "..." string: letters-only body, so a digit inside is #1 and EOF inside is #3.
  std::vector<scilex::rule> string_grammar()
  {
    std::vector<scilex::rule> rules;
    rules.push_back({1, real::regex("[a-z]+")});
    rules.push_back({2, real::regex(R"(\s+)"), true});
    rules.push_back(acting(in_mode_rule(3, "\"", {}), act_push("str")));
    rules.push_back(in_mode_rule(4, "[a-z]+", {"str"}));
    rules.push_back(acting(in_mode_rule(5, "\"", {"str"}), act_pop()));
    return rules;
  }

  // (...) groups: the opener is active in default AND in a group, so it nests.
  std::vector<scilex::rule> nest_grammar()
  {
    std::vector<scilex::rule> rules;
    rules.push_back(acting(in_mode_rule(1, R"(\()", {"default", "grp"}), act_push("grp")));
    rules.push_back(acting(in_mode_rule(2, R"(\))", {"grp"}), act_pop()));
    rules.push_back(in_mode_rule(3, "[a-z]+", {"grp"}));
    rules.push_back({4, real::regex(R"(\s+)"), true});
    return rules;
  }

  // A pop active at the root: popping there is #2 (via the shared apply_transition).
  std::vector<scilex::rule> root_pop_grammar()
  {
    std::vector<scilex::rule> rules;
    rules.push_back({1, real::regex("[a-z]+")});
    rules.push_back(acting(in_mode_rule(2, R"(\))", {}), act_pop()));
    return rules;
  }

  // "#...\n" via set (no push): depth never grows, so EOF is clean (no #3); the \n
  // exercises the reference's line/column advance.
  std::vector<scilex::rule> set_grammar()
  {
    std::vector<scilex::rule> rules;
    rules.push_back({1, real::regex("[a-z]+")});
    rules.push_back(acting(in_mode_rule(2, "#", {}), act_set("cmt")));
    rules.push_back(in_mode_rule(3, R"([^\n]+)", {"cmt"}));
    rules.push_back(acting(in_mode_rule(4, R"(\n)", {"cmt"}), act_set("default")));
    return rules;
  }

  // <<...>> raw block: the delimiters are skipped yet still drive push/pop.
  std::vector<scilex::rule> raw_grammar()
  {
    std::vector<scilex::rule> rules;
    rules.push_back({1, real::regex("[a-z]+")});
    rules.push_back(acting(in_mode_rule(2, "<<", {}, true), act_push("raw")));
    rules.push_back(in_mode_rule(3, R"([^>]+)", {"raw"}));
    rules.push_back(acting(in_mode_rule(4, ">>", {"raw"}, true), act_pop()));
    return rules;
  }

  //! \brief A multi-mode grammar with crafted inputs (balanced, nesting, the three errors).
  struct mode_grammar
  {
    std::string_view              name;
    std::vector<scilex::rule>     rules;
    std::vector<std::string_view> inputs;
  };

  std::vector<mode_grammar> mode_grammars()
  {
    std::vector<mode_grammar> out;
    out.push_back({"mm.string", string_grammar(),
                   {"ab cd", "\"ab\"", "x \"ab\" y", "\"\"", "\"ab", "\"a1\""}});
    out.push_back({"mm.nest", nest_grammar(),
                   {"(a(b)c)", "(a(b))", "(a", "((", ")", ""}});
    out.push_back({"mm.rootpop", root_pop_grammar(), {"ab", "a)", ")"}});
    out.push_back({"mm.set", set_grammar(), {"ab#xy\ncd", "ab#xy", "#\nab"}});
    out.push_back({"mm.raw", raw_grammar(), {"xy", "a<<bc>>d", "<<bc"}});
    return out;
  }

  int run()
  {
    int         failures {0};
    std::size_t cases    {0};
    for (const grammar& gram : grammars) {
      const std::vector<scilex::rule> rules {gram.rules()};
      const scilex::lexer             lex   {gram.lexer()};

      std::vector<std::string> inputs;
      inputs.emplace_back(gram.sample);
      for (std::size_t n {0}; n <= gram.sample.size(); ++n) {
        inputs.emplace_back(gram.sample.substr(0, n)); // every truncation
      }
      for (const std::string& bad : adversarial) {
        inputs.push_back(bad);
      }

      for (const std::string& input : inputs) {
        ++cases;
        const scilex::fuzz::result outcome {scilex::fuzz::check(rules, lex, input, gram.has_layout)};
        if (!outcome.ok) {
          std::cerr << "FAIL [" << gram.name << "] " << outcome.invariant << '\n'
                    << "  input (" << input.size() << " bytes): " << preview(input) << '\n';
          ++failures;
        }
      }

      // Error recovery (error_policy::token): the token lexer must equal the recovery reference on the
      // benign inputs above AND the dense adversarial corpus. dfa_modes = {"default"} exercises the DFA
      // scan path where the mode accelerates and Pike elsewhere — both must agree on the ERROR tokens.
      const scilex::lexer token_lex {gram.rules(), {}, {"default"}, scilex::error_policy::token};
      for (const std::string& input : inputs) {
        ++cases;
        const scilex::fuzz::result outcome {scilex::fuzz::check_recover(rules, token_lex, input)};
        if (!outcome.ok) {
          std::cerr << "FAIL [" << gram.name << " token] " << outcome.invariant << '\n'
                    << "  input (" << input.size() << " bytes): " << preview(input) << '\n';
          ++failures;
        }
      }
      for (const std::string& input : token_corpus) {
        ++cases;
        const scilex::fuzz::result outcome {scilex::fuzz::check_recover(rules, token_lex, input)};
        if (!outcome.ok) {
          std::cerr << "FAIL [" << gram.name << " token] " << outcome.invariant << '\n'
                    << "  input (" << input.size() << " bytes): " << preview(input) << '\n';
          ++failures;
        }
      }
    }

    // Multi-mode grammars: the per-mode dispatch vs the brute-force reference.
    for (const mode_grammar& gram : mode_grammars()) {
      const scilex::lexer lex       {gram.rules};
      const scilex::lexer token_lex {gram.rules, {}, {}, scilex::error_policy::token};
      for (const std::string_view input : gram.inputs) {
        cases += 2;
        const scilex::fuzz::result outcome {scilex::fuzz::check(gram.rules, lex, input, false)};
        if (!outcome.ok) {
          std::cerr << "FAIL [" << gram.name << "] " << outcome.invariant << '\n'
                    << "  input (" << input.size() << " bytes): " << preview(input) << '\n';
          ++failures;
        }
        // Recovery on the crafted #2/#3/#4 cases: pop-at-root and zero-length stay fatal (both throw
        // together), EOF-inside-a-mode yields the zero-width ERROR — the recovery reference pins it all.
        const scilex::fuzz::result recovered {scilex::fuzz::check_recover(gram.rules, token_lex, input)};
        if (!recovered.ok) {
          std::cerr << "FAIL [" << gram.name << " token] " << recovered.invariant << '\n'
                    << "  input (" << input.size() << " bytes): " << preview(input) << '\n';
          ++failures;
        }
      }
    }
    if (failures == 0) {
      std::cout << "fuzz-check: " << cases << " cases (9 grammars x sample/truncations/adversarial, raise + token recovery, + 5 multi-mode grammars) — all invariants hold\n";
      return 0;
    }
    std::cerr << "fuzz-check: " << failures << " invariant violation(s)\n";
    return 1;
  }
} // namespace

int main()
{
  return run();
}
