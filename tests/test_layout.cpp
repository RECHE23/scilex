// The indentation layout pass: NEWLINE / INDENT / DEDENT insertion from token
// positions, with blank/comment lines ignored and inconsistent dedents caught.
#include <string_view>
#include <vector>

#include <sciforge/test/framework.hpp>
#include "scilex/scilex.hpp"
#include "scilex/layout.hpp"

namespace {

  inline constexpr int ws      {0};
  inline constexpr int comment {1};
  inline constexpr int id      {2};

  scilex::lexer make_lexer()
  {
    std::vector<scilex::rule> rules;
    rules.push_back({.kind = ws, .pattern = real::regex("\\s+"), .skip = true});
    rules.push_back({.kind = comment, .pattern = real::regex("#[^\n]*"), .skip = true});
    rules.push_back({.kind = id, .pattern = real::regex("[a-z]+"), .skip = false});
    return scilex::lexer(std::move(rules));
  }

  // The kinds of the laid-out token stream for a source.
  std::vector<int> layout_kinds(std::string_view source)
  {
    const scilex::lexer lexer  {make_lexer()};
    const auto          tokens {lexer.tokenize(source, scilex::eof_policy::append)};
    std::vector<int>    kinds;
    for (const scilex::token& tok : scilex::layout(tokens)) {
      kinds.push_back(tok.kind);
    }
    return kinds;
  }
} // namespace

TEST(single_line_gets_a_trailing_newline)
{
  const std::vector<int> expected {id, id, scilex::newline, scilex::end_of_input};
  EXPECT(layout_kinds("a b") == expected);
}

TEST(empty_source_is_just_end_of_input)
{
  const std::vector<int> expected {scilex::end_of_input};
  EXPECT(layout_kinds("") == expected);
}

TEST(indentation_inserts_indent_and_dedent)
{
  // a
  //   b
  //   c
  // d
  const std::vector<int> expected {id, scilex::newline,
                                   scilex::indent, id, scilex::newline,
                                   id, scilex::newline,
                                   scilex::dedent, id, scilex::newline,
                                   scilex::end_of_input};
  EXPECT(layout_kinds("a\n  b\n  c\nd") == expected);
}

TEST(deeper_nesting_emits_multiple_dedents)
{
  // a
  //   b
  //     c
  // d   <- two levels closed at once
  const std::vector<int> expected {id, scilex::newline,
                                   scilex::indent, id, scilex::newline,
                                   scilex::indent, id, scilex::newline,
                                   scilex::dedent, scilex::dedent, id, scilex::newline,
                                   scilex::end_of_input};
  EXPECT(layout_kinds("a\n  b\n    c\nd") == expected);
}

TEST(blank_and_comment_lines_carry_no_structure)
{
  // a
  //            <- blank
  //   # note   <- comment only
  //   b        <- ends indented, so a trailing DEDENT closes the block
  const std::vector<int> expected {id, scilex::newline,
                                   scilex::indent, id, scilex::newline,
                                   scilex::dedent,
                                   scilex::end_of_input};
  EXPECT(layout_kinds("a\n\n  # note\n  b\n") == expected);
}

TEST(inconsistent_dedent_is_an_error)
{
  // a
  //     b   (indent 4)
  //   c     (indent 2: matches no open level)
  bool threw {false};
  try {
    const auto kinds {layout_kinds("a\n    b\n  c")};
    (void)kinds;
  }
  catch (const scilex::layout_error& error) {
    threw = true;
    EXPECT_EQ(error.where().line, 3U); // the offending line `c`
  }
  EXPECT(threw);
}

namespace {

  inline constexpr int text {3};
  inline constexpr int op   {4};
  inline constexpr int nl   {5};

  // A grammar with a token that spans lines (a triple-quoted string) and, optionally, a newline that
  // is a real token rather than skipped whitespace.
  std::vector<int> spanning_kinds(std::string_view source,
                                  bool             newline_is_a_token)
  {
    std::vector<scilex::rule> rules;
    rules.push_back({.kind = ws, .pattern = real::regex("[ ]+"), .skip = true});
    rules.push_back({.kind = nl, .pattern = real::regex("\n"), .skip = !newline_is_a_token});
    rules.push_back({.kind = text, .pattern = real::regex(R"rx((?s)""".*?""")rx"), .skip = false});
    rules.push_back({.kind = id, .pattern = real::regex("[a-z]+"), .skip = false});
    rules.push_back({.kind = op, .pattern = real::regex("[=+]"), .skip = false});
    const scilex::lexer lexer  {std::move(rules)};
    const auto          tokens {lexer.tokenize(source, scilex::eof_policy::append)};
    std::vector<int>    kinds;
    for (const scilex::token& tok : scilex::layout(tokens)) {
      kinds.push_back(tok.kind);
    }
    return kinds;
  }
} // namespace

// A token that spans lines leaves the scan on its CLOSING line: what follows it there continues the
// logical line, even when the closing line is indented deeper than the opening one.
TEST(a_token_spanning_lines_does_not_open_a_line_where_it_closes)
{
  const std::vector<int> expected {id, op, text, op, id, scilex::newline, id, scilex::newline, scilex::end_of_input};
  EXPECT(spanning_kinds("x = \"\"\"a\nb\"\"\" + y\nz\n", false) == expected);
  EXPECT(spanning_kinds("x = \"\"\"a\n    b\"\"\" + y\nz\n", false) == expected);
}

// A token that ENDS with a newline ends its line: the token after it opens the next one.
TEST(a_token_ending_with_a_newline_ends_its_line)
{
  const std::vector<int> expected {id, nl, scilex::newline, id, nl, scilex::newline, scilex::end_of_input};
  EXPECT(spanning_kinds("a\nb\n", true) == expected);
}
