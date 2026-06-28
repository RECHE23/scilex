// The lexer: maximal-munch tokenization, rule priority, skipping, position
// tracking, empty-match safety and lexical errors.
#include <string_view>
#include <type_traits>
#include <vector>

#include <sciforge/test/framework.hpp>
#include "scilex/scilex.hpp"

using namespace std::string_view_literals;

namespace {

  // The lexer stores indices (not pointers) into its own rules_, so it is a value
  // type: copy and move are both well-defined (a copy tokenizes identically).
  static_assert(std::is_copy_constructible_v<scilex::lexer>, "lexer should be copyable");
  static_assert(std::is_copy_assignable_v<scilex::lexer>, "lexer should be copy-assignable");
  static_assert(std::is_move_constructible_v<scilex::lexer>, "lexer should stay movable");

  // Token kinds shared by the tests (plain ints: a token's kind is an int).
  inline constexpr int WS    {0};
  inline constexpr int KW_IF {1};
  inline constexpr int ID    {2};
  inline constexpr int NUM   {3};
  inline constexpr int PLUS  {4};

  // A small expression-language lexer: whitespace (skipped), the keyword `if`
  // (before identifiers, so it wins ties), identifiers, integers, and `+`.
  scilex::lexer make_lexer()
  {
    std::vector<scilex::rule> rules;
    rules.push_back({.kind = WS, .pattern = real::regex("\\s+"), .skip = true});
    rules.push_back({.kind = KW_IF, .pattern = real::regex("if"), .skip = false});
    rules.push_back({.kind = ID, .pattern = real::regex("[a-z]+"), .skip = false});
    rules.push_back({.kind = NUM, .pattern = real::regex("[0-9]+"), .skip = false});
    rules.push_back({.kind = PLUS, .pattern = real::regex("\\+"), .skip = false});
    return scilex::lexer(std::move(rules));
  }
} // namespace

TEST(tokenizes_and_skips_whitespace)
{
  const auto tokens = make_lexer().tokenize("if x + 42");
  EXPECT_EQ(tokens.size(), 4U); // whitespace omitted
  EXPECT_EQ(tokens[0].kind, KW_IF);
  EXPECT_EQ(tokens[0].lexeme, "if"sv);
  EXPECT_EQ(tokens[1].kind, ID);
  EXPECT_EQ(tokens[1].lexeme, "x"sv);
  EXPECT_EQ(tokens[2].kind, PLUS);
  EXPECT_EQ(tokens[2].lexeme, "+"sv);
  EXPECT_EQ(tokens[3].kind, NUM);
  EXPECT_EQ(tokens[3].lexeme, "42"sv);
}

TEST(maximal_munch_beats_priority)
{
  // "ifx" is one identifier, not the keyword `if` followed by `x`: the longer
  // match wins even though the keyword rule comes first.
  const auto tokens = make_lexer().tokenize("ifx");
  EXPECT_EQ(tokens.size(), 1U);
  EXPECT_EQ(tokens[0].kind, ID);
  EXPECT_EQ(tokens[0].lexeme, "ifx"sv);
}

TEST(priority_breaks_equal_length_ties)
{
  // "if" matches both the keyword (len 2) and the identifier (len 2); the
  // earlier rule (keyword) wins the tie.
  const auto tokens = make_lexer().tokenize("if");
  EXPECT_EQ(tokens.size(), 1U);
  EXPECT_EQ(tokens[0].kind, KW_IF);
}

TEST(tracks_line_and_column)
{
  const auto tokens = make_lexer().tokenize("ab\n  cd");
  EXPECT_EQ(tokens.size(), 2U);
  EXPECT_EQ(tokens[0].start.line, 1U);
  EXPECT_EQ(tokens[0].start.column, 1U);
  EXPECT_EQ(tokens[1].lexeme, "cd"sv);
  EXPECT_EQ(tokens[1].start.line, 2U);   // after the newline
  EXPECT_EQ(tokens[1].start.column, 3U); // two leading spaces
  EXPECT_EQ(tokens[1].start.offset, 5U);
}

TEST(unmatched_input_throws_with_position)
{
  bool threw {false};
  try {
    const auto tokens = make_lexer().tokenize("x @");
    (void)tokens;
  }
  catch (const scilex::lex_error& error) {
    threw = true;
    EXPECT_EQ(error.where().offset, 2U); // the '@'
    EXPECT_EQ(error.where().column, 3U);
  }
  EXPECT(threw);
}

TEST(empty_matches_never_stall_the_scan)
{
  // A nullable rule ([0-9]*) matches the empty string; the lexer must never loop
  // forever on it — it consumes a non-empty run normally, and a zero-length win
  // (nothing else matches here) surfaces as a positioned error instead of a hang.
  std::vector<scilex::rule> rules;
  rules.push_back({.kind = NUM, .pattern = real::regex("[0-9]*"), .skip = false});
  const scilex::lexer lx(std::move(rules));
  EXPECT_EQ(lx.tokenize("123").size(), 1U);             // consumes the digits
  EXPECT_THROWS(lx.tokenize("abc"), scilex::lex_error); // does not hang
}

TEST(zero_length_winning_match_is_a_positioned_error)
{
  // (a) A nullable rule ([0-9]*) is the only rule; at a non-digit byte it "wins" with a
  // zero-length match. Advancing by 0 would spin forever, so the shared guard reports a
  // positioned lex_error naming the cause — distinct from the #1 "no rule matches".
  std::vector<scilex::rule> rules;
  rules.push_back({.kind = NUM, .pattern = real::regex("[0-9]*"), .skip = false});
  const scilex::lexer lx(std::move(rules));
  bool                threw {false};
  try {
    (void)lx.tokenize("x"); // [0-9]* matches the empty string at 'x'
  }
  catch (const scilex::lex_error& error) {
    threw = true;
    EXPECT(std::string_view {error.what()}.find("zero-length") != std::string_view::npos);
    EXPECT_EQ(error.where().offset, 0U); // at the offending byte
  }
  EXPECT(threw);
}

TEST(nullable_rule_matching_non_empty_still_lexes)
{
  // (b) Non-regression: only a zero-length WIN errors. The same nullable rule consumes a
  // non-empty run exactly as before — maximal munch is untouched for >0 matches.
  std::vector<scilex::rule> rules;
  rules.push_back({.kind = NUM, .pattern = real::regex("[0-9]*"), .skip = false});
  const scilex::lexer lx(std::move(rules));
  const auto          tokens {lx.tokenize("007")};
  EXPECT_EQ(tokens.size(), 1U);
  EXPECT_EQ(tokens[0].kind, NUM);
  EXPECT_EQ(tokens[0].lexeme, "007"sv);
}
