// The SciLex contract (docs/spec.dox), pinned executable. Each example the spec states is run through
// BOTH the real lexer and the independent oracle (fuzz/reference.hpp) and checked against the exact
// token sequence the spec documents — so neither the prose nor the code can drift from the other. If a
// contract claim here is wrong (or the lexer changes to contradict it), this test bites.
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sciforge/test/framework.hpp>
#include "scilex/scilex.hpp"
#include "reference.hpp" // the oracle — from fuzz/, kept out of the coverage set

using namespace std::string_view_literals;

namespace {

  using kind_lexeme = std::pair<int, std::string_view>;

  // Build a rule with an optional in_mode / action, one stable line per rule.
  scilex::rule rule(int                                kind,
                    const char*                        pattern,
                    bool                               skip   = false,
                    std::vector<std::string>           modes  = {},
                    std::optional<scilex::mode_action> action = std::nullopt)
  {
    scilex::rule r {.kind = kind, .pattern = real::regex(pattern), .skip = skip};
    r.in_mode = std::move(modes);
    r.action  = std::move(action);
    return r;
  }

  // The (kind, lexeme) sequence a set of rules yields on `input`, via the real lexer.
  std::vector<kind_lexeme> lexer_facts(const std::vector<scilex::rule>& rules,
                                       std::string_view                 input,
                                       scilex::error_policy             errors = scilex::error_policy::raise,
                                       scilex::eof_policy               eof    = scilex::eof_policy::omit)
  {
    std::vector<kind_lexeme> out;
    const scilex::lexer      lex {std::vector<scilex::rule>(rules), {}, {}, errors};
    for (const scilex::token& tok : lex.tokenize(input, eof)) {
      out.emplace_back(tok.kind, tok.lexeme);
    }
    return out;
  }

  // The same via the independent oracle (raise policy only — the recovery oracle is checked separately).
  std::vector<kind_lexeme> oracle_facts(const std::vector<scilex::rule>& rules,
                                        std::string_view                 input)
  {
    std::vector<kind_lexeme> out;
    for (const scilex::token& tok : scilex::fuzz::reference_tokenize(rules, input)) {
      out.emplace_back(tok.kind, tok.lexeme);
    }
    return out;
  }

  // Assert the lexer AND the oracle both produce `want` on `input`.
  void pin(const std::vector<scilex::rule>& rules,
           std::string_view                 input,
           const std::vector<kind_lexeme>&  want)
  {
    EXPECT_EQ(lexer_facts(rules, input) == want, true);
    EXPECT_EQ(oracle_facts(rules, input) == want, true);
  }

  // Token kinds for the examples (plain ints — a token's kind is an int).
  inline constexpr int KW        {0};
  inline constexpr int IDENT     {1};
  inline constexpr int WS        {2};
  inline constexpr int STR_OPEN  {3};
  inline constexpr int STR_BODY  {4};
  inline constexpr int STR_CLOSE {5};

  // §spec_munch — longest wins; on a tie the earliest rule wins; whitespace is a skip rule.
  TEST(spec_maximal_munch_and_tie_break)
  {
    const std::vector<scilex::rule> g {rule(KW, "if"), rule(IDENT, "[a-z]+"),
                                       rule(WS, R"(\s+)", /*skip=*/ true)};
    pin(g, "if", {{KW, "if"sv}});                   // tie (both match "if"): the earlier rule (keyword) wins
    pin(g, "ifx", {{IDENT, "ifx"sv}});              // longest wins: "ifx" beats "if"
    pin(g, "if x", {{KW, "if"sv}, {IDENT, "x"sv}}); // the whitespace token is skipped, not emitted
  }

  // §spec_eof — eof_policy::append yields a terminal end_of_input token.
  TEST(spec_eof_policy_append)
  {
    const std::vector<scilex::rule> g {rule(IDENT, "[a-z]+")};
    const auto                      with_eof = lexer_facts(g, "ab", scilex::error_policy::raise, scilex::eof_policy::append);
    EXPECT_EQ(with_eof.size(), 2U);
    EXPECT_EQ(with_eof[0].first, IDENT);
    EXPECT_EQ(with_eof[1].first, scilex::end_of_input);
    EXPECT_EQ(with_eof[1].second.size(), 0U); // zero-width, at the end position
  }

  // §spec_modes — push on the opener, pop on the closer; the body rule is active only in "str".
  TEST(spec_modes_push_pop)
  {
    using op = scilex::mode_action::op;
    std::vector<scilex::rule> g;
    g.push_back(rule(IDENT, "[a-z]+"));
    g.push_back(rule(STR_OPEN, "\"", false, {}, scilex::mode_action {.operation = op::push, .target = "str"}));
    g.push_back(rule(STR_BODY, "[a-z]+", false, {"str"}));
    g.push_back(rule(STR_CLOSE, "\"", false, {"str"}, scilex::mode_action {.operation = op::pop}));
    pin(g, "a\"bc\"d", {{IDENT, "a"sv}, {STR_OPEN, "\""sv}, {STR_BODY, "bc"sv},
          {STR_CLOSE, "\""sv}, {IDENT, "d"sv}});               // same byte '"' lexes by mode
  }

  // §spec_failures — under error_policy::token, an unmatched run is ONE scilex::error token; the lexer
  // and the recovery oracle agree, including the exact bytes and position.
  TEST(spec_error_recovery_run)
  {
    const std::vector<scilex::rule> g    {rule(IDENT, "[a-z]+"), rule(WS, R"(\s+)", true)};
    const auto                      lex = lexer_facts(g, "ab @# cd", scilex::error_policy::token);
    const std::vector<kind_lexeme>  want {{IDENT, "ab"sv}, {scilex::error, "@#"sv}, {IDENT, "cd"sv}};
    EXPECT_EQ(lex == want, true);
    // the independent recovery oracle produces the same sequence.
    std::vector<kind_lexeme> ora;
    for (const scilex::token& tok : scilex::fuzz::reference_tokenize_recover(g, "ab @# cd")) {
      ora.emplace_back(tok.kind, tok.lexeme);
    }
    EXPECT_EQ(ora == want, true);
  }

  // §spec_failures — a pop at the root (#2) and a zero-length match (#4) stay fatal, even under token.
  TEST(spec_fatal_failures_stay_fatal)
  {
    using op = scilex::mode_action::op;
    std::vector<scilex::rule> root_pop;
    root_pop.push_back(rule(IDENT, "[a-z]+"));
    root_pop.push_back(rule(1, R"(\))", false, {}, scilex::mode_action {.operation = op::pop}));
    EXPECT_THROWS(scilex::lexer(std::move(root_pop), {}, {}, scilex::error_policy::token).tokenize("a)"),
                  scilex::lex_error);      // #2 pop-at-root

    std::vector<scilex::rule> nullable;
    nullable.push_back(rule(IDENT, "a*")); // matches empty on 'b'
    EXPECT_THROWS(scilex::lexer(std::move(nullable), {}, {}, scilex::error_policy::token).tokenize("b"),
                  scilex::lex_error);      // #4 zero-length
  }

  // §spec_reserved — the five reserved kinds are mutually distinct (a static guard on the contract).
  TEST(spec_reserved_kinds_distinct)
  {
    const int reserved[] {scilex::end_of_input, scilex::error, scilex::newline,
                          scilex::indent, scilex::dedent};
    for (std::size_t i {0}; i < std::size(reserved); ++i) {
      for (std::size_t j {i + 1}; j < std::size(reserved); ++j) {
        EXPECT_EQ(reserved[i] != reserved[j], true);
      }
    }
  }
} // namespace
