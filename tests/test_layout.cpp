// The indentation layout pass: NEWLINE / INDENT / DEDENT insertion from token
// positions, with blank/comment lines ignored and inconsistent dedents caught.
#include <string_view>
#include <vector>

#include "framework.hpp"
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
