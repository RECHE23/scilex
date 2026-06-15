// The lazy token iterator: range-for, explicit increment, equivalence with
// tokenize, empty input, early stop, and error propagation while iterating.
#include <string_view>
#include <vector>

#include "framework.hpp"
#include "scilex/scilex.hpp"

using namespace std::string_view_literals;

namespace {

  inline constexpr int WS    {0};
  inline constexpr int KW_IF {1};
  inline constexpr int ID    {2};
  inline constexpr int NUM   {3};
  inline constexpr int PLUS  {4};

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

TEST(lazy_scan_matches_eager_tokenize)
{
  const scilex::lexer lx           {make_lexer()};
  const auto          source       {"if x + 42\n  y"sv};

  const auto                 eager {lx.tokenize(source)};
  std::vector<scilex::token> lazy;
  for (const scilex::token& tok : lx.scan(source)) {
    lazy.push_back(tok);
  }

  EXPECT_EQ(lazy.size(), eager.size());
  bool same {lazy.size() == eager.size()};
  for (std::size_t i {0}; same && i < lazy.size(); ++i) {
    same = lazy[i].kind == eager[i].kind && lazy[i].lexeme == eager[i].lexeme
           && lazy[i].start.offset == eager[i].start.offset
           && lazy[i].start.line == eager[i].start.line
           && lazy[i].start.column == eager[i].start.column;
  }
  EXPECT(same);
}

TEST(explicit_iteration_and_end_sentinel)
{
  const scilex::lexer lx    {make_lexer()};
  const auto          range {lx.scan("if x")};
  auto                it    {range.begin()};
  EXPECT(it != range.end());
  EXPECT_EQ((*it).kind, KW_IF);
  EXPECT_EQ(it->lexeme, "if"sv); // operator->
  ++it;
  EXPECT_EQ(it->kind, ID);
  EXPECT_EQ(it->lexeme, "x"sv);
  ++it;
  EXPECT(it == range.end()); // exhausted
}

TEST(post_increment_and_advance_past_end_are_safe)
{
  const scilex::lexer lx    {make_lexer()};
  const auto          range {lx.scan("a b")};
  auto                it    {range.begin()};
  EXPECT_EQ(it->lexeme, "a"sv);
  it++;                      // post-increment
  EXPECT_EQ(it->lexeme, "b"sv);
  it++;
  EXPECT(it == range.end()); // exhausted
  ++it;                      // advancing past the end is a safe no-op
  EXPECT(it == range.end());
}

TEST(iterator_equality_compares_position)
{
  const scilex::lexer lx    {make_lexer()};
  const auto          range {lx.scan("a b c")};
  auto                first {range.begin()};
  auto                same  {range.begin()};
  EXPECT(first == same); // two live iterators at the same offset
  ++same;
  EXPECT(first != same); // now at different offsets
}

TEST(empty_source_yields_empty_range)
{
  const scilex::lexer lx    {make_lexer()};
  const auto          range {lx.scan("")};
  EXPECT(range.begin() == range.end());

  std::size_t count {0};
  for (const auto& tok : range) {
    (void)tok;
    ++count;
  }
  EXPECT_EQ(count, 0U);
}

TEST(iteration_can_stop_early)
{
  // Laziness in practice: take only the first token of a long source without
  // scanning the rest.
  const scilex::lexer lx    {make_lexer()};
  const auto          range {lx.scan("aaa bbb ccc ddd eee")};
  auto                it    {range.begin()};
  EXPECT_EQ(it->lexeme, "aaa"sv);
  // Stop here — no requirement to consume the remaining tokens.
  EXPECT(it != range.end());
}

TEST(error_propagates_while_iterating)
{
  const scilex::lexer lx    {make_lexer()};
  const auto          range {lx.scan("ab @")};
  auto                it    {range.begin()};
  EXPECT_EQ(it->lexeme, "ab"sv); // first token is fine

  bool threw {false};
  try {
    ++it; // advancing onto '@' fails
  }
  catch (const scilex::lex_error& error) {
    threw = true;
    EXPECT_EQ(error.where().offset, 3U);
  }
  EXPECT(threw);
}
