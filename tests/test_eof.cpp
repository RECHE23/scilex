// The synthetic end-of-input token: appended on demand by tokenize() and
// scan(), carrying the real end position (past trailing trivia), with the
// reserved end_of_input kind. This is what a parser's cursor matches against.
#include <string_view>
#include <vector>

#include <sciforge/test/framework.hpp>
#include "scilex/scilex.hpp"

using namespace std::string_view_literals;

namespace {

  inline constexpr int WS  {0};
  inline constexpr int ID  {1};
  inline constexpr int NUM {2};

  scilex::lexer make_lexer()
  {
    std::vector<scilex::rule> rules;
    rules.push_back({.kind = WS, .pattern = real::regex("\\s+"), .skip = true});
    rules.push_back({.kind = ID, .pattern = real::regex("[a-z]+"), .skip = false});
    rules.push_back({.kind = NUM, .pattern = real::regex("[0-9]+"), .skip = false});
    return scilex::lexer(std::move(rules));
  }
} // namespace

TEST(tokenize_omits_eof_by_default)
{
  const auto tokens = make_lexer().tokenize("if x");
  EXPECT_EQ(tokens.size(), 2U);
  EXPECT_EQ(tokens.back().kind, ID); // no end_of_input token
}

TEST(tokenize_appends_eof_with_end_position)
{
  const auto tokens = make_lexer().tokenize("if x", scilex::eof_policy::append);
  EXPECT_EQ(tokens.size(), 3U); // two ids + end_of_input
  const scilex::token& last = tokens.back();
  EXPECT_EQ(last.kind, scilex::end_of_input);
  EXPECT(last.lexeme.empty());
  EXPECT_EQ(last.start.offset, 4U); // one past the last byte
  EXPECT_EQ(last.start.line, 1U);
  EXPECT_EQ(last.start.column, 5U);
}

TEST(eof_on_empty_source)
{
  const auto tokens = make_lexer().tokenize("", scilex::eof_policy::append);
  EXPECT_EQ(tokens.size(), 1U);
  EXPECT_EQ(tokens[0].kind, scilex::end_of_input);
  EXPECT_EQ(tokens[0].start.offset, 0U);
  EXPECT_EQ(tokens[0].start.line, 1U);
  EXPECT_EQ(tokens[0].start.column, 1U);
}

TEST(eof_position_is_past_trailing_trivia)
{
  // The end position is after the trailing newline, not at the last token.
  const auto tokens = make_lexer().tokenize("a\n", scilex::eof_policy::append);
  EXPECT_EQ(tokens.size(), 2U); // "a" + end_of_input
  const scilex::token& last = tokens.back();
  EXPECT_EQ(last.kind, scilex::end_of_input);
  EXPECT_EQ(last.start.offset, 2U);
  EXPECT_EQ(last.start.line, 2U);   // after the newline
  EXPECT_EQ(last.start.column, 1U);
}

TEST(scan_yields_eof_as_last_token)
{
  const scilex::lexer        lx {make_lexer()};
  std::vector<scilex::token> tokens;
  for (const scilex::token& tok : lx.scan("a b", scilex::eof_policy::append)) {
    tokens.push_back(tok);
  }
  EXPECT_EQ(tokens.size(), 3U); // a, b, end_of_input
  EXPECT_EQ(tokens.back().kind, scilex::end_of_input);
  EXPECT_EQ(tokens.back().start.offset, 3U);
}

TEST(scan_eof_on_empty_source)
{
  const scilex::lexer lx    {make_lexer()};
  std::size_t         count {0};
  scilex::token       last  {};
  for (const scilex::token& tok : lx.scan("", scilex::eof_policy::append)) {
    last = tok;
    ++count;
  }
  EXPECT_EQ(count, 1U);
  EXPECT_EQ(last.kind, scilex::end_of_input);
}

TEST(scan_omits_eof_by_default)
{
  const scilex::lexer lx    {make_lexer()};
  std::size_t         count {0};
  for (const scilex::token& tok : lx.scan("a b")) {
    (void)tok;
    ++count;
  }
  EXPECT_EQ(count, 2U); // no end_of_input token
}

TEST(end_of_input_is_distinct_from_user_kinds)
{
  EXPECT(scilex::end_of_input != WS);
  EXPECT(scilex::end_of_input != ID);
  EXPECT(scilex::end_of_input != NUM);
}
