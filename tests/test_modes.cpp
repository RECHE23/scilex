// Contextual lexing — modes (Slice 1). Part 1: construction + build validation.
// Part 2: the mode machine — push/pop/set applied during a scan via a per-scan mode
// stack, with apply_transition the pure pivot (shared verbatim by the fuzz oracle).
// Mono-mode rule sets keep building and scanning exactly as before.
#include <cstddef>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "framework.hpp"
#include "scilex/scilex.hpp"

using namespace std::string_view_literals;

namespace {

  scilex::mode_action push_to(const char* mode)
  {
    return {.operation = scilex::mode_action::op::push, .target = mode};
  }

  scilex::mode_action set_to(const char* mode)
  {
    return {.operation = scilex::mode_action::op::set, .target = mode};
  }

  scilex::mode_action pop_mode()
  {
    return {.operation = scilex::mode_action::op::pop};
  }

  scilex::position pos(std::size_t offset,
                       std::size_t line,
                       std::size_t column)
  {
    return {.offset = offset, .line = line, .column = column};
  }

  // Builds a rule with an in_mode list and/or an action without a long one-line
  // designated init (uncrustify aligns multi-line designated inits into a thrash).
  scilex::rule mode_rule(int                      kind,
                         const char             * pattern,
                         std::vector<std::string> in_mode,
                         bool                     skip = false)
  {
    scilex::rule r {.kind = kind, .pattern = real::regex(pattern), .skip = skip};
    r.in_mode = std::move(in_mode);
    return r;
  }

  scilex::rule with_action(scilex::rule               r,
                           const scilex::mode_action& action)
  {
    r.action = action;
    return r;
  }

  // A string literal opens a "str" mode (its body is one mode) and closes it.
  std::vector<scilex::rule> string_modes()
  {
    std::vector<scilex::rule> rules;
    rules.push_back({.kind = 1, .pattern = real::regex("[a-z]+")});                 // default ident
    rules.push_back(with_action(mode_rule(2, "\"", {}), push_to("str")));           // open
    rules.push_back(mode_rule(3, "[^\"]+", {"str"}));                               // body
    rules.push_back(with_action(mode_rule(4, "\"", {"str"}), pop_mode()));          // close
    return rules;
  }
} // namespace

// --- Part 1: construction + build validation -------------------------------------

TEST(multi_mode_grammar_builds_and_default_mode_still_scans)
{
  const scilex::lexer              lexer(string_modes());
  const std::vector<scilex::token> tokens {lexer.tokenize("abc")};
  EXPECT_EQ(tokens.size(), 1U);
  EXPECT_EQ(tokens[0].kind, 1);
  EXPECT_EQ(tokens[0].lexeme, "abc"sv);
}

TEST(build_rejects_a_transition_rule_with_an_empty_pattern)
{
  std::vector<scilex::rule> rules;
  rules.push_back({.kind = 1, .pattern = real::regex("[a-z]+")});
  rules.push_back(with_action(mode_rule(2, "", {}), push_to("x")));
  rules.push_back(mode_rule(3, "y", {"x"}));
  EXPECT_THROWS(scilex::lexer {std::move(rules)}, std::invalid_argument);
}

TEST(build_rejects_a_transition_into_an_empty_mode)
{
  std::vector<scilex::rule> rules;
  rules.push_back({.kind = 1, .pattern = real::regex("[a-z]+")});
  rules.push_back(with_action(mode_rule(2, "\"", {}), push_to("ghost")));
  EXPECT_THROWS(scilex::lexer {std::move(rules)}, std::invalid_argument);
}

TEST(build_accepts_a_transition_into_a_mode_whose_rule_is_nullable)
{
  std::vector<scilex::rule> rules;
  rules.push_back({.kind = 1, .pattern = real::regex("[a-z]+")});
  rules.push_back(with_action(mode_rule(2, "\"", {}), push_to("m")));
  rules.push_back(mode_rule(3, "[0-9]*", {"m"})); // nullable → general (still "non-empty")
  const scilex::lexer lexer(std::move(rules));
  EXPECT_EQ(lexer.tokenize("abc").size(), 1U);
}

// --- Part 2: the mode machine ----------------------------------------------------

TEST(push_then_pop_lexes_each_mode_in_turn)
{
  const scilex::lexer lexer(string_modes());
  const auto          tokens = lexer.tokenize("ab\"cd\"ef");
  EXPECT_EQ((std::vector<int> {tokens[0].kind, tokens[1].kind, tokens[2].kind,
                               tokens[3].kind, tokens[4].kind}),
            (std::vector<int> {1, 2, 3, 4, 1}));
  EXPECT_EQ(tokens[2].lexeme, "cd"sv); // body lexed in "str"
  EXPECT_EQ(tokens[4].lexeme, "ef"sv); // back in default after the pop
}

TEST(modes_nest_via_the_stack)
{
  // "(" opens a group in default OR in a group (nesting); ")" pops one level.
  std::vector<scilex::rule> rules;
  rules.push_back(with_action(mode_rule(1, "\\(", {"default", "grp"}), push_to("grp")));
  rules.push_back(with_action(mode_rule(2, "\\)", {"grp"}), pop_mode()));
  rules.push_back(mode_rule(3, "[a-z]+", {"grp"}));
  const scilex::lexer lexer(std::move(rules));
  const auto          tokens = lexer.tokenize("(a(b))");
  EXPECT_EQ(tokens.size(), 6U); // ( a ( b ) ) — balanced, no error
  EXPECT_EQ(tokens[2].kind, 1); // the nested "("
  EXPECT_EQ(tokens[3].lexeme, "b"sv);
}

TEST(set_replaces_the_active_mode_without_changing_depth)
{
  // "#" switches to a comment mode in place; a newline switches back. No push/pop,
  // so the stack never deepens and end-of-input is clean (no unterminated error).
  std::vector<scilex::rule> rules;
  rules.push_back({.kind = 1, .pattern = real::regex("[a-z]+")});
  rules.push_back(with_action(mode_rule(2, "#", {}), set_to("cmt")));
  rules.push_back(mode_rule(3, "[^\n]+", {"cmt"}));
  rules.push_back(with_action(mode_rule(4, "\n", {"cmt"}), set_to("default")));
  const scilex::lexer lexer(std::move(rules));
  const auto          tokens = lexer.tokenize("ab#xy\ncd");
  EXPECT_EQ(tokens.size(), 5U);
  EXPECT_EQ(tokens[2].kind, 3);        // "xy" lexed in the comment mode
  EXPECT_EQ(tokens[4].lexeme, "cd"sv); // back in default
}

TEST(a_skip_rule_can_still_drive_a_transition)
{
  // <<…>> raw block: the delimiters transition but are skipped (not emitted).
  std::vector<scilex::rule> rules;
  rules.push_back({.kind = 1, .pattern = real::regex("[a-z]+")});
  rules.push_back(with_action(mode_rule(0, "<<", {}, /*skip=*/ true), push_to("raw")));
  rules.push_back(mode_rule(2, "[^>]+", {"raw"}));
  rules.push_back(with_action(mode_rule(0, ">>", {"raw"}, /*skip=*/ true), pop_mode()));
  const scilex::lexer lexer(std::move(rules));
  const auto          tokens = lexer.tokenize("a<<bc>>d");
  EXPECT_EQ(tokens.size(), 3U);        // a, bc, d — the << and >> are skipped
  EXPECT_EQ(tokens[1].kind, 2);
  EXPECT_EQ(tokens[1].lexeme, "bc"sv);
  EXPECT_EQ(tokens[2].lexeme, "d"sv);  // back in default after the skipped pop
}

TEST(a_skip_rule_can_still_drive_a_set_transition)
{
  // A skipped delimiter that *sets* the mode in place: /…/ switches to an "alt"
  // number mode and back, the slashes consumed but not emitted (skip + set).
  std::vector<scilex::rule> rules;
  rules.push_back({.kind = 1, .pattern = real::regex("[a-z]+")});
  rules.push_back(with_action(mode_rule(0, "/", {}, /*skip=*/ true), set_to("alt")));
  rules.push_back(mode_rule(2, "[0-9]+", {"alt"}));
  rules.push_back(with_action(mode_rule(0, "/", {"alt"}, /*skip=*/ true), set_to("default")));
  const scilex::lexer lexer(std::move(rules));
  const auto          tokens = lexer.tokenize("ab/12/cd");
  EXPECT_EQ(tokens.size(), 3U);        // ab, 12, cd — the slashes are skipped
  EXPECT_EQ(tokens[1].kind, 2);        // "12" lexed in the alt mode
  EXPECT_EQ(tokens[1].lexeme, "12"sv);
  EXPECT_EQ(tokens[2].lexeme, "cd"sv); // back in default
}

TEST(error_no_rule_in_mode_points_at_the_byte_and_names_the_mode)
{
  // In "num" only digits match; a letter there fails with a positioned #1 error.
  std::vector<scilex::rule> rules;
  rules.push_back(with_action(mode_rule(1, "#", {}), push_to("num")));
  rules.push_back(mode_rule(2, "[0-9]+", {"num"}));
  const scilex::lexer lexer(std::move(rules));
  try {
    (void)lexer.tokenize("#a");
    EXPECT(false);                       // unreachable
  }
  catch (const scilex::lex_error& error) {
    EXPECT_EQ(error.where().offset, 1U); // the 'a'
    EXPECT(std::string {error.what()}.find("num") != std::string::npos);
  }
}

TEST(error_pop_at_root_is_reported)
{
  // A pop active in the default (root) mode has nothing to leave.
  std::vector<scilex::rule> rules;
  rules.push_back(with_action(mode_rule(1, "\\)", {}), pop_mode()));
  const scilex::lexer lexer(std::move(rules));
  EXPECT_THROWS(lexer.tokenize(")"), scilex::lex_error);
}

TEST(error_unterminated_mode_points_at_the_opening)
{
  // Input ends inside a pushed mode: #3 reports the opening position.
  const scilex::lexer lexer(string_modes());
  try {
    (void)lexer.tokenize("\"abc");       // opens "str" at 1:1, never closes
    EXPECT(false);                       // unreachable
  }
  catch (const scilex::lex_error& error) {
    EXPECT_EQ(error.where().offset, 0U); // the opening quote
    EXPECT(std::string {error.what()}.find("unterminated") != std::string::npos);
    EXPECT(std::string {error.what()}.find("str") != std::string::npos);
  }
}

TEST(apply_transition_is_a_pure_stack_operation)
{
  const std::map<std::string, std::size_t> mode_id {{"default", 0}, {"a", 1}, {"b", 2}};
  std::vector<scilex::frame>               stack   {scilex::frame {.mode_id = 0, .entry_pos = pos(0, 1, 1)}};

  scilex::apply_transition(with_action(mode_rule(0, "x", {}), push_to("a")), pos(5, 1, 6), stack, mode_id);
  EXPECT_EQ(stack.size(), 2U);
  EXPECT_EQ(stack.back().mode_id, 1U);
  EXPECT_EQ(stack.back().entry_pos.offset, 5U); // push remembers the start

  scilex::apply_transition(with_action(mode_rule(0, "x", {}), set_to("b")), pos(7, 1, 8), stack, mode_id);
  EXPECT_EQ(stack.size(), 2U);                  // set keeps the depth
  EXPECT_EQ(stack.back().mode_id, 2U);

  scilex::apply_transition(with_action(mode_rule(0, "x", {}), pop_mode()), pos(9, 1, 10), stack, mode_id);
  EXPECT_EQ(stack.size(), 1U);
  EXPECT_EQ(stack.back().mode_id, 0U);

  // A rule without an action is a no-op; a pop at the root throws (#2).
  scilex::apply_transition({.kind = 0, .pattern = real::regex("x")}, pos(0, 1, 1), stack, mode_id);
  EXPECT_EQ(stack.size(), 1U);
  EXPECT_THROWS(scilex::apply_transition(with_action(mode_rule(0, "x", {}), pop_mode()),
                                         pos(9, 1, 10), stack, mode_id),
                scilex::lex_error);
}
