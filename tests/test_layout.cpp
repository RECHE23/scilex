// The indentation layout pass: NEWLINE / INDENT / DEDENT insertion from token
// positions, with blank/comment lines ignored and inconsistent dedents caught.
#include <stdexcept>
#include <string>
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

namespace {
  // The layout under a tab policy, or the message of the layout_error it raised.
  struct tab_outcome
  {
    std::vector<int> kinds;
    std::string      error;
  };

  tab_outcome layout_with_tabs(std::string_view   source,
                               scilex::tab_policy tabs)
  {
    const scilex::lexer lexer  {make_lexer()};
    const auto          tokens {lexer.tokenize(source, scilex::eof_policy::append)};
    tab_outcome         outcome;
    try {
      for (const scilex::token& tok : scilex::layout(tokens, source, tabs)) {
        outcome.kinds.push_back(tok.kind);
      }
    }
    catch (const scilex::layout_error& error) {
      outcome.error = error.what();
    }
    return outcome;
  }

  const std::string mixed {"inconsistent use of tabs and spaces in indentation"};
} // namespace

TEST(columns_policy_with_a_source_is_the_positional_pass)
{
  for (const std::string_view source : {"a\n  b\n  c\nd\n", "a\n\tb\n        c\n", "a\n\tb\n\tc\n", "x"}) {
    const scilex::lexer lexer  {make_lexer()};
    const auto          tokens {lexer.tokenize(source, scilex::eof_policy::append)};
    const auto          plain  {scilex::layout(tokens)};
    const auto          with   {scilex::layout(tokens, source, scilex::tab_policy::columns)};
    EXPECT(plain.size() == with.size());
    for (std::size_t i = 0; i < plain.size() && i < with.size(); ++i) {
      EXPECT(plain[i].kind == with[i].kind);
    }
  }
}

TEST(python_policy_measures_a_tab_to_the_next_stop_of_eight)
{
  // A tab then eight spaces: one level under CPython's width (8 == 8) -- but only because the tab is
  // 8 wide; counted as 1 it is shallower, so the level is ambiguous.
  EXPECT(layout_with_tabs("a\n\tb\n        c\n", scilex::tab_policy::python).error == mixed);
  // The same text under the columns policy is simply deeper (1 then 8).
  EXPECT(layout_with_tabs("a\n\tb\n        c\n", scilex::tab_policy::columns).error.empty());

  // Tabs throughout are consistent: the usual INDENT / DEDENT.
  const std::vector<int> expected {id, scilex::newline,
                                   scilex::indent, id, scilex::newline,
                                   scilex::indent, id, scilex::newline,
                                   scilex::dedent, scilex::dedent, id, scilex::newline,
                                   scilex::end_of_input};
  const tab_outcome tabs_only {layout_with_tabs("a\n\tb\n\t\tc\nd\n", scilex::tab_policy::python)};
  EXPECT(tabs_only.error.empty() && tabs_only.kinds == expected);
}

TEST(python_policy_refuses_each_ambiguous_comparison)
{
  // Deeper by tab stops, not deeper counting a tab as 1: four spaces, then a tab (8 vs 4, 1 vs 4).
  EXPECT(layout_with_tabs("a\n    b\n\tc\n", scilex::tab_policy::python).error == mixed);
  // Equal by tab stops, unequal counting tabs as 1: "  \t" and "\t" both reach column 8.
  EXPECT(layout_with_tabs("a\n  \tb\n\tc\n", scilex::tab_policy::python).error == mixed);
  // A dedent that lands on a level by tab stops but not by count: the outer level is "\t" (8, 1),
  // the line is eight spaces (8, 8).
  EXPECT(layout_with_tabs("a\n\tb\n\t\tc\n        d\n", scilex::tab_policy::python).error == mixed);
  // A dedent to no level at all keeps its own message.
  EXPECT(layout_with_tabs("a\n    b\n  c\n", scilex::tab_policy::python).error == "inconsistent indentation");
  // Mixed but unambiguous is accepted: a tab then a space is deeper than a tab by both measures.
  EXPECT(layout_with_tabs("a\n\tb\n\t c\n", scilex::tab_policy::python).error.empty());
}

TEST(python_policy_resets_at_a_form_feed)
{
  // CPython resets both measures at a form feed, so "\f  b" sits at column 2 by both.
  const tab_outcome outcome {layout_with_tabs("a\n\t\f  b\n", scilex::tab_policy::python)};
  EXPECT(outcome.error.empty());
  EXPECT(outcome.kinds.size() > 3 && outcome.kinds[2] == scilex::indent);
}

TEST(python_policy_refuses_a_source_shorter_than_the_tokens)
{
  const scilex::lexer lexer  {make_lexer()};
  const auto          tokens {lexer.tokenize("a\n  b\n", scilex::eof_policy::append)};
  bool                threw  {false};
  try {
    static_cast<void>(scilex::layout(tokens, "a", scilex::tab_policy::python));
  }
  catch (const std::invalid_argument&) {
    threw = true;
  }
  EXPECT(threw);
}
