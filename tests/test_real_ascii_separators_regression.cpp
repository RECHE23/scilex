// Regression: real-regex v2026.7.37 fixed ASCII-mode `\s` (real::flags::ascii) wrongly including
// the four control separators U+001C-U+001F (FS/GS/RS/US) -- a grammar table `\s`/`\S` pair
// wrongly classified them as whitespace under real < 2026.7.37. SciLex pins real::flags::ascii on
// every DFA-compat grammar (see test_dfa_modes.cpp's plain() helper), so this bug directly flipped
// this library's own tokenization of these four bytes: a lexer that skips `\s+` as layout and
// tokenizes everything else as `\S+` would silently swallow FS/GS/RS/US as skipped whitespace and
// split an otherwise-contiguous run of "other" bytes around them, instead of keeping them as
// ordinary non-space content in one token. No arc/version-gated code here by design -- this test
// asserts the CORRECT (post-fix) behaviour outright; its own history (see the fiche this regression
// was written for) proves it fails against a real-regex checkout older than 2026.7.37 by pin-flip
// (building against an isolated worktree at the pre-fix commit), not by a runtime version check.
#include <string_view>

#include <sciforge/test/framework.hpp>
#include "scilex/scilex.hpp"

using namespace std::string_view_literals;

namespace {

  inline constexpr int WS    {0};
  inline constexpr int OTHER {1};

  // \s+ (skipped, layout) and \S+ (everything else) under real::flags::ascii -- the exact pin
  // SciLex's DFA-compat grammars use. Maximal-munch means \S+ greedily consumes every
  // non-whitespace byte in a run, including FS/GS/RS/US once they correctly stop being \s.
  scilex::lexer make_lexer()
  {
    std::vector<scilex::rule> rules;
    rules.push_back({.kind = WS, .pattern = real::regex(R"(\s+)", real::flags::ascii), .skip = true});
    rules.push_back({.kind = OTHER, .pattern = real::regex(R"(\S+)", real::flags::ascii), .skip = false});
    return scilex::lexer(std::move(rules));
  }
} // namespace

TEST(ascii_separators_are_not_whitespace)
{
  // "a" + FS + "b": FS (U+001C) is NOT ascii whitespace post-fix, so \S+ greedily consumes all
  // three bytes as ONE token. Pre-fix, FS was wrongly \s, splitting this into two tokens ("a", "b")
  // with FS silently skipped as layout.
  const auto tokens = make_lexer().tokenize("a\x1c" "b");
  EXPECT_EQ(tokens.size(), 1U);
  if (tokens.size() != 1) {
    return; // guard: a size mismatch (e.g. against a pre-fix real-regex) must not index out of bounds
  }
  EXPECT_EQ(tokens[0].kind, OTHER);
  EXPECT_EQ(tokens[0].lexeme, "a\x1c" "b"sv);
}

TEST(all_four_ascii_separators_are_not_whitespace)
{
  // FS/GS/RS/US (U+001C-U+001F), all four, sandwiched between real ASCII whitespace on both
  // sides -- each must stay part of the \S+ run it sits in, not get skipped as \s.
  const auto tokens = make_lexer().tokenize(" \x1c\x1d\x1e\x1f ");
  EXPECT_EQ(tokens.size(), 1U); // the leading/trailing real spaces are skipped; the run in between is one token
  if (tokens.size() != 1) {
    return; // guard: a size mismatch (e.g. against a pre-fix real-regex) must not index out of bounds
  }
  EXPECT_EQ(tokens[0].kind, OTHER);
  EXPECT_EQ(tokens[0].lexeme, "\x1c\x1d\x1e\x1f"sv);
}

TEST(ordinary_ascii_whitespace_still_skips)
{
  // Control: real ASCII whitespace (space, tab) is unaffected by the fix -- still skipped.
  const auto tokens = make_lexer().tokenize("a \t b");
  EXPECT_EQ(tokens.size(), 2U);
  if (tokens.size() != 2) {
    return; // guard: keep this control robust to the same class of size mismatch as the two above
  }
  EXPECT_EQ(tokens[0].lexeme, "a"sv);
  EXPECT_EQ(tokens[1].lexeme, "b"sv);
}
