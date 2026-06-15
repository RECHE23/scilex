// The lexer: maximal-munch tokenization, rule priority, skipping, position
// tracking, empty-match safety and lexical errors.
#include <string_view>
#include <vector>

#include "framework.hpp"
#include "scilex/scilex.hpp"

using namespace std::string_view_literals;

namespace {

  // Token kinds shared by the tests.
  enum kind
  {
    WS,
    KW_IF,
    ID,
    NUM,
    PLUS
  };

  // A small expression-language lexer: whitespace (skipped), the keyword `if`
  // (before identifiers, so it wins ties), identifiers, integers, and `+`.
  scilex::lexer make_lexer()
  {
    std::vector<scilex::rule> rules;
    rules.push_back({WS, real::regex("\\s+"), true});
    rules.push_back({KW_IF, real::regex("if"), false});
    rules.push_back({ID, real::regex("[a-z]+"), false});
    rules.push_back({NUM, real::regex("[0-9]+"), false});
    rules.push_back({PLUS, real::regex("\\+"), false});
    return scilex::lexer(std::move(rules));
  }
} // namespace

TEST(tokenizes_and_skips_whitespace)
{
  const auto tokens = make_lexer().tokenize("if x + 42");
  EXPECT_EQ(tokens.size(), 4U); // whitespace omitted
  EXPECT_EQ(tokens[0].kind, static_cast<int>(KW_IF));
  EXPECT_EQ(tokens[0].lexeme, "if"sv);
  EXPECT_EQ(tokens[1].kind, static_cast<int>(ID));
  EXPECT_EQ(tokens[1].lexeme, "x"sv);
  EXPECT_EQ(tokens[2].kind, static_cast<int>(PLUS));
  EXPECT_EQ(tokens[2].lexeme, "+"sv);
  EXPECT_EQ(tokens[3].kind, static_cast<int>(NUM));
  EXPECT_EQ(tokens[3].lexeme, "42"sv);
}

TEST(maximal_munch_beats_priority)
{
  // "ifx" is one identifier, not the keyword `if` followed by `x`: the longer
  // match wins even though the keyword rule comes first.
  const auto tokens = make_lexer().tokenize("ifx");
  EXPECT_EQ(tokens.size(), 1U);
  EXPECT_EQ(tokens[0].kind, static_cast<int>(ID));
  EXPECT_EQ(tokens[0].lexeme, "ifx"sv);
}

TEST(priority_breaks_equal_length_ties)
{
  // "if" matches both the keyword (len 2) and the identifier (len 2); the
  // earlier rule (keyword) wins the tie.
  const auto tokens = make_lexer().tokenize("if");
  EXPECT_EQ(tokens.size(), 1U);
  EXPECT_EQ(tokens[0].kind, static_cast<int>(KW_IF));
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
  // A nullable rule ([0-9]*) matches the empty string; the lexer must ignore
  // such matches rather than loop forever, and fall back to a real error when
  // nothing consumes input.
  std::vector<scilex::rule> rules;
  rules.push_back({NUM, real::regex("[0-9]*"), false});
  const scilex::lexer lx(std::move(rules));
  EXPECT_EQ(lx.tokenize("123").size(), 1U);             // consumes the digits
  EXPECT_THROWS(lx.tokenize("abc"), scilex::lex_error); // does not hang
}
