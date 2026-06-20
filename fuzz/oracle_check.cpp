/*!
 * \file oracle_check.cpp
 * \brief Deterministic, seeded gate over the fuzzing oracle (\ref reference.hpp).
 *
 * Runs every property invariant for each of the seven example grammars across a
 * fixed adversarial input set — the sample, every prefix of it (truncations cut
 * tokens mid-stream), and shared hostile strings (empty, NUL/control/high bytes,
 * invalid UTF-8, unmatched delimiters, an unterminated string, long runs). This
 * is the bounded, repeatable half of ③ — wired into `make fuzz-check` /
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
    }
    if (failures == 0) {
      std::cout << "fuzz-check: " << cases << " cases (7 grammars x sample/truncations/adversarial) — all invariants hold\n";
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
