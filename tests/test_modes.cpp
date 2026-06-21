// Contextual lexing — mode construction and build-time validation (Slice 1, part 1).
// The mode machine (push/pop/set applied during a scan) arrives in part 2; here we
// pin that a multi-mode rule set BUILDS correctly (mode interning + a per-mode
// dispatch) and that the build rejects ill-formed transitions up front. A plain
// rule set keeps building and scanning exactly as before (mono-mode unchanged).
#include <stdexcept>
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

  scilex::mode_action pop_mode()
  {
    return {.operation = scilex::mode_action::op::pop};
  }

  // A valid two-mode grammar: a string literal opens a "str" mode and closes it.
  // (In part 1 the scan stays in "default"; this only exercises construction.)
  std::vector<scilex::rule> string_modes()
  {
    std::vector<scilex::rule> rules;
    rules.push_back({.kind = 1, .pattern = real::regex("[a-z]+")});                                       // default ident
    rules.push_back({.kind = 2, .pattern = real::regex("\""), .action = push_to("str")});                 // open
    rules.push_back({.kind = 3, .pattern = real::regex("[^\"]+"), .in_mode = {"str"}});                   // body
    rules.push_back({.kind = 4, .pattern = real::regex("\""), .in_mode = {"str"}, .action = pop_mode()}); // close
    return rules;
  }
} // namespace

TEST(multi_mode_grammar_builds_and_default_mode_still_scans)
{
  // Construction interns "default" + "str", builds a dispatch per mode, and validates
  // every transition — all without throwing. Mode "str" is reused across two rules
  // (the second interning hits the already-present path). Scanning stays in default
  // for now and tokenizes plain input exactly as a mono-mode lexer would.
  const scilex::lexer              lexer(string_modes());
  const std::vector<scilex::token> tokens {lexer.tokenize("abc")};
  EXPECT_EQ(tokens.size(), 1U);
  EXPECT_EQ(tokens[0].kind, 1);
  EXPECT_EQ(tokens[0].lexeme, "abc"sv);
}

TEST(build_rejects_a_transition_rule_with_an_empty_pattern)
{
  // A transition must consume input; a rule matching the empty string would fire its
  // transition without advancing.
  std::vector<scilex::rule> rules;
  rules.push_back({.kind = 1, .pattern = real::regex("[a-z]+")});
  rules.push_back({.kind = 2, .pattern = real::regex(""), .action = push_to("x")});
  rules.push_back({.kind = 3, .pattern = real::regex("y"), .in_mode = {"x"}});
  EXPECT_THROWS(scilex::lexer {std::move(rules)}, std::invalid_argument);
}

TEST(build_rejects_a_transition_into_an_empty_mode)
{
  // Pushing into a mode no rule is active in would dead-end the scan; caught early.
  std::vector<scilex::rule> rules;
  rules.push_back({.kind = 1, .pattern = real::regex("[a-z]+")});
  rules.push_back({.kind = 2, .pattern = real::regex("\""), .action = push_to("ghost")});
  EXPECT_THROWS(scilex::lexer {std::move(rules)}, std::invalid_argument);
}

TEST(build_accepts_a_transition_into_a_mode_whose_rule_is_nullable)
{
  // The non-empty-target check counts a nullable rule too: it lands in the mode's
  // general list (not a first-byte bucket), so the mode is still "non-empty".
  std::vector<scilex::rule> rules;
  rules.push_back({.kind = 1, .pattern = real::regex("[a-z]+")});
  rules.push_back({.kind = 2, .pattern = real::regex("\""), .action = push_to("m")});
  rules.push_back({.kind = 3, .pattern = real::regex("[0-9]*"), .in_mode = {"m"}}); // nullable → general
  const scilex::lexer lexer(std::move(rules));
  EXPECT_EQ(lexer.tokenize("abc").size(), 1U);                                      // builds; default-mode scan unchanged
}
