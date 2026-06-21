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
#include "scilex/layout.hpp"
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

// --- Layout Awareness: a characterization of the current model's limit ----------

TEST(flow_multiline_produces_spurious_indent_with_current_layout)
{
  // CHARACTERIZATION (a limit, not a capability): layout() is positional and
  // mode-blind, so a multi-line flow collection picks up INDENT/DEDENT from its
  // inner lines' columns. This is the EXPECTED behaviour of the current decoupled
  // model; Layout Awareness Level A (flow = insignificant) will remove these and
  // this test will then be updated. See examples/yaml.hpp + include/scilex/layout.hpp.
  std::vector<scilex::rule> rules;
  rules.push_back(with_action(mode_rule(1, R"([{\[])", {"default", "flow"}), push_to("flow")));
  rules.push_back(with_action(mode_rule(2, R"([}\]])", {"flow"}), pop_mode()));
  rules.push_back(mode_rule(0, R"(\s+)", {"default", "flow"}, /*skip=*/ true));
  rules.push_back(mode_rule(3, ":", {"flow"}));
  rules.push_back(mode_rule(4, ",", {"flow"}));
  rules.push_back(mode_rule(5, R"([^\s:,{}\[\]]+)", {"flow"}));
  const scilex::lexer              lexer(std::move(rules));
  const std::vector<scilex::token> flat    {lexer.tokenize("{\n a: 1,\n b: 2\n}", scilex::eof_policy::append)};
  const std::vector<scilex::token> laid    {scilex::layout(flat)};
  int                              indents {0};
  int                              dedents {0};
  for (const scilex::token& tok : laid) {
    if (tok.kind == scilex::indent) {
      ++indents;
    }
    else if (tok.kind == scilex::dedent) {
      ++dedents;
    }
  }
  EXPECT(indents > 0); // spurious: the flow body's inner lines look like a block
  EXPECT_EQ(indents, dedents);
}

// --- Layout Awareness Level A: the machine ---------------------------------------

namespace {
  // The same flow grammar built twice below: default (significant) + a "flow" mode.
  std::vector<scilex::rule> flow_grammar()
  {
    std::vector<scilex::rule> rules;
    rules.push_back(with_action(mode_rule(1, R"([{\[])", {"default", "flow"}), push_to("flow")));
    rules.push_back(with_action(mode_rule(2, R"([}\]])", {"flow"}), pop_mode()));
    rules.push_back(mode_rule(0, R"(\s+)", {"default", "flow"}, /*skip=*/ true));
    rules.push_back(mode_rule(3, R"([^\s{}\[\]]+)", {"flow"}));
    return rules;
  }

  int indent_dedent_count(const std::vector<scilex::token>& tokens)
  {
    int count {0};
    for (const scilex::token& tok : tokens) {
      count += static_cast<int>(tok.kind == scilex::indent || tok.kind == scilex::dedent);
    }
    return count;
  }
} // namespace

TEST(layout_awareness_suppresses_an_insignificant_mode)
{
  // The same grammar, scanned identically (A never touches the scan). Without a
  // policy a multi-line flow body emits spurious INDENT/DEDENT; with "flow" marked
  // insignificant it emits none, while the default-mode tokens still shape layout.
  const std::string_view src {"{\n a\n b\n}"};

  const scilex::lexer plain(flow_grammar());
  EXPECT(plain.mode_significant().empty()); // no policy
  EXPECT(indent_dedent_count(scilex::layout(plain.tokenize(src, scilex::eof_policy::append))) > 0);

  const scilex::lexer aware(flow_grammar(), {"flow"});
  EXPECT(!aware.mode_significant().empty());
  EXPECT(aware.mode_name(1) == "flow");    // "default" is 0, "flow" is 1
  const std::vector<scilex::token> laid {
    scilex::layout(aware.tokenize(src, scilex::eof_policy::append), aware.mode_significant())};
  EXPECT_EQ(indent_dedent_count(laid), 0); // the flow body added no structure
}

TEST(an_empty_significance_policy_matches_the_positional_layout)
{
  // Invariant 1: with no insignificant modes the policy is empty, and layout() with
  // it equals the positional pass (the default-argument overload) token-for-token.
  const scilex::lexer lexer(string_modes()); // default + str, none insignificant
  EXPECT(lexer.mode_significant().empty());
  const std::vector<scilex::token> toks        {lexer.tokenize("ab\"x\ny\"cd", scilex::eof_policy::append)};
  const std::vector<scilex::token> positional  {scilex::layout(toks)};
  const std::vector<scilex::token> with_policy {scilex::layout(toks, lexer.mode_significant())};
  EXPECT_EQ(positional.size(), with_policy.size());
  bool identical                               {positional.size() == with_policy.size()};
  for (std::size_t i {0}; identical && i < positional.size(); ++i) {
    identical = positional[i].kind == with_policy[i].kind
                && positional[i].start.offset == with_policy[i].start.offset;
  }
  EXPECT(identical);
}

TEST(a_mode_id_beyond_the_significance_policy_is_significant)
{
  // layout()'s guard: a token whose mode_id is past the policy is treated as
  // significant, so a short policy never indexes out of bounds.
  const std::vector<scilex::token> toks {
    scilex::token {.kind = 1, .lexeme = "a"sv, .start = pos(0, 1, 1), .mode_id = 1},
    scilex::token {.kind = 1, .lexeme = "b"sv, .start = pos(4, 2, 3), .mode_id = 1},
    scilex::token {.kind = scilex::end_of_input, .lexeme = {}, .start = pos(5, 2, 4), .mode_id = 0},
  };
  const std::vector<bool> policy {false}; // size 1 (mode 0 only); mode 1 is beyond it
  EXPECT(indent_dedent_count(scilex::layout(toks, policy)) > 0);
}

TEST(build_rejects_an_unknown_insignificant_mode)
{
  // An insignificant-mode name must be a mode the rules actually use.
  EXPECT_THROWS(scilex::lexer(string_modes(), {"nonexistent"}), std::invalid_argument);
}
