// Error recovery (scilex::error_policy::token): a maximal run of bytes that no rule in the active
// mode can begin is emitted as one reserved `scilex::error` token (its lexeme the exact bytes) rather
// than thrown; a pushed mode still open at end of input yields one zero-width error token at the EOF;
// and the fatal cases (a pop at the root, a zero-length match) still throw under either policy. The
// default policy (raise) is unchanged — the rest of the suite pins that.
#include <string>
#include <string_view>
#include <vector>

#include <sciforge/test/framework.hpp>
#include "scilex/scilex.hpp"
#include "scilex/layout.hpp"

using namespace std::string_view_literals;

namespace {

  inline constexpr int WORD {1};
  inline constexpr int WS   {2};

  // A default-mode grammar: lowercase words, whitespace skipped. Anything else is error text.
  scilex::lexer word_lexer(scilex::error_policy            policy,
                           std::vector<std::string>        dfa = {})
  {
    std::vector<scilex::rule> rules;
    rules.push_back({.kind = WORD, .pattern = real::regex("[a-z]+")});
    rules.push_back({.kind = WS, .pattern = real::regex(R"(\s+)"), .skip = true});
    return scilex::lexer {std::move(rules), {}, std::move(dfa), policy};
  }

  TEST(recovers_a_simple_no_match_run)
  {
    const auto toks = word_lexer(scilex::error_policy::token).tokenize("ab @# cd");
    EXPECT_EQ(toks.size(), 3U);
    EXPECT_EQ(toks[0].kind, WORD);
    EXPECT_EQ(toks[0].lexeme, "ab"sv);
    EXPECT_EQ(toks[1].kind, scilex::error); // one error token for the whole "@#" run
    EXPECT_EQ(toks[1].lexeme, "@#"sv);
    EXPECT_EQ(toks[1].start.offset, 3U);
    EXPECT_EQ(toks[2].kind, WORD);
    EXPECT_EQ(toks[2].lexeme, "cd"sv);
  }

  TEST(error_lexeme_is_the_exact_bytes)
  {
    const std::string in {"ab\x01\x02\x7f""cd"}; // control bytes between two words
    const auto        toks = word_lexer(scilex::error_policy::token).tokenize(in);
    EXPECT_EQ(toks.size(), 3U);
    EXPECT_EQ(toks[1].kind, scilex::error);
    EXPECT_EQ(toks[1].lexeme, std::string_view(in).substr(2, 3)); // exactly the 3 control bytes
  }

  TEST(recovers_invalid_utf8_run)
  {
    const std::string in(6, '\xff');
    const auto        toks = word_lexer(scilex::error_policy::token).tokenize(in);
    EXPECT_EQ(toks.size(), 1U);
    EXPECT_EQ(toks[0].kind, scilex::error);
    EXPECT_EQ(toks[0].lexeme.size(), 6U); // the whole invalid run is one error token
  }

  TEST(recovers_binary_bytes_interleaved)
  {
    const std::string in {"x\x00\x01y"sv}; // NUL among words (string_view literal keeps the NUL)
    const auto        toks = word_lexer(scilex::error_policy::token).tokenize(in);
    EXPECT_EQ(toks.size(), 3U);
    EXPECT_EQ(toks[0].lexeme, "x"sv);
    EXPECT_EQ(toks[1].kind, scilex::error);
    EXPECT_EQ(toks[1].lexeme.size(), 2U); // the NUL + \x01
    EXPECT_EQ(toks[2].lexeme, "y"sv);
  }

  TEST(error_run_ends_at_a_line_start)
  {
    // A newline is whitespace (a match), so the error run stops before it; the next word is on line 2.
    const auto toks = word_lexer(scilex::error_policy::token).tokenize("@@\nok");
    EXPECT_EQ(toks.size(), 2U);
    EXPECT_EQ(toks[0].kind, scilex::error);
    EXPECT_EQ(toks[0].lexeme, "@@"sv);
    EXPECT_EQ(toks[1].lexeme, "ok"sv);
    EXPECT_EQ(toks[1].start.line, 2U);
  }

  TEST(the_dfa_scan_path_recovers_identically)
  {
    // Same grammar, default mode accelerated by a DFA: the ERROR tokens must match the Pike path.
    const std::string in                                             {"ab @# cd"};
    const auto        pike = word_lexer(scilex::error_policy::token).tokenize(in);
    const auto        dfa  = word_lexer(scilex::error_policy::token, {"default"}).tokenize(in);
    EXPECT_EQ(dfa.size(), pike.size());
    for (std::size_t i {0}; i < dfa.size(); ++i) {
      EXPECT_EQ(dfa[i].kind, pike[i].kind);
      EXPECT_EQ(dfa[i].lexeme, pike[i].lexeme);
      EXPECT_EQ(dfa[i].start.offset, pike[i].start.offset);
    }
  }

  TEST(lazy_scan_equals_eager_with_errors)
  {
    scilex::lexer              lex {word_lexer(scilex::error_policy::token)};
    const std::string          in  {"ab @# cd !! ef"};
    const auto                 eager = lex.tokenize(in);
    std::vector<scilex::token> lazy;
    for (const scilex::token& tok : lex.scan(in)) {
      lazy.push_back(tok);
    }
    EXPECT_EQ(lazy.size(), eager.size());
    for (std::size_t i {0}; i < eager.size(); ++i) {
      EXPECT_EQ(lazy[i].kind, eager[i].kind);
      EXPECT_EQ(lazy[i].lexeme, eager[i].lexeme);
    }
  }

  TEST(recovery_is_deterministic)
  {
    scilex::lexer     lex {word_lexer(scilex::error_policy::token)};
    const std::string in  {"@@ ab ## cd"};
    const auto        a = lex.tokenize(in);
    const auto        b = lex.tokenize(in);
    EXPECT_EQ(a.size(), b.size());
    for (std::size_t i {0}; i < a.size(); ++i) {
      EXPECT_EQ(a[i].kind, b[i].kind);
      EXPECT_EQ(a[i].lexeme, b[i].lexeme);
    }
  }

  TEST(a_run_continues_past_a_first_byte_that_does_not_complete)
  {
    // Rule "foo": a bare 'f' is a valid first byte (the pre-filter admits it) but does not complete a
    // match, so the error run correctly continues past it and stops only at the real "foo".
    std::vector<scilex::rule> rules;
    rules.push_back({.kind = 1, .pattern = real::regex("foo")});
    scilex::lexer lex {std::move(rules), {}, {}, scilex::error_policy::token};
    const auto    toks = lex.tokenize("xfoyfoo");
    EXPECT_EQ(toks.size(), 2U);
    EXPECT_EQ(toks[0].kind, scilex::error);
    EXPECT_EQ(toks[0].lexeme, "xfoy"sv); // the lone 'f' at offset 1 did not stop the run
    EXPECT_EQ(toks[1].kind, 1);
    EXPECT_EQ(toks[1].lexeme, "foo"sv);
  }

  TEST(raise_policy_is_unchanged)
  {
    // The default still throws at the first unmatched byte — recovery is strictly opt-in.
    EXPECT_THROWS(word_lexer(scilex::error_policy::raise).tokenize("ab @# cd"), scilex::lex_error);
  }

  // --- mode grammars: a "..." string mode (letters only inside), pushed on the quote --------------

  scilex::lexer string_lexer(scilex::error_policy policy)
  {
    std::vector<scilex::rule> rules;
    rules.push_back({.kind = WORD, .pattern = real::regex("[a-z]+")});
    rules.push_back({.kind = WS, .pattern = real::regex(R"(\s+)"), .skip = true});
    scilex::rule open {.kind = 3, .pattern = real::regex("\"")};
    open.action = scilex::mode_action {.operation = scilex::mode_action::op::push, .target = "str"};
    rules.push_back(open);
    rules.push_back({.kind = 4, .pattern = real::regex("[a-z]+"), .in_mode = {"str"}});
    scilex::rule close {.kind = 5, .pattern = real::regex("\""), .in_mode = {"str"}};
    close.action = scilex::mode_action {.operation = scilex::mode_action::op::pop};
    rules.push_back(close);
    return scilex::lexer {std::move(rules), {}, {}, policy};
  }

  TEST(recovers_inside_a_pushed_mode_then_closes)
  {
    // A digit inside the string mode has no rule; recovery emits an error, and the closing quote —
    // found mid-run — ends the run and pops back to the default mode.
    const auto toks = string_lexer(scilex::error_policy::token).tokenize("\"a1b\"c");
    // " (push) · a · 1 (error) · b · " (pop) · c
    EXPECT_EQ(toks.size(), 6U);
    EXPECT_EQ(toks[2].kind, scilex::error);
    EXPECT_EQ(toks[2].lexeme, "1"sv);
    EXPECT_EQ(toks[5].kind, WORD); // back in the default mode
    EXPECT_EQ(toks[5].lexeme, "c"sv);
  }

  TEST(eof_inside_a_pushed_mode_yields_a_zero_width_error)
  {
    // The string is never closed: recovery emits one zero-width error at the EOF, keeping the partial
    // tokens already emitted, instead of throwing #3.
    const std::string in {"\"ab"};
    const auto        toks = string_lexer(scilex::error_policy::token).tokenize(in);
    EXPECT_EQ(toks.size(), 3U);                 // " · ab · <zero-width error>
    EXPECT_EQ(toks[2].kind, scilex::error);
    EXPECT_EQ(toks[2].lexeme.size(), 0U);
    EXPECT_EQ(toks[2].start.offset, in.size()); // positioned at the EOF
  }

  TEST(nested_modes_recover_and_stay_in_mode)
  {
    // '(' pushes a group mode (nestable); a bad byte inside recovers without leaving the mode.
    std::vector<scilex::rule> rules;
    scilex::rule              open {.kind = 1, .pattern = real::regex(R"(\()"), .in_mode = {"default", "grp"}};
    open.action = scilex::mode_action {.operation = scilex::mode_action::op::push, .target = "grp"};
    rules.push_back(open);
    scilex::rule close {.kind = 2, .pattern = real::regex(R"(\))"), .in_mode = {"grp"}};
    close.action = scilex::mode_action {.operation = scilex::mode_action::op::pop};
    rules.push_back(close);
    rules.push_back({.kind = 3, .pattern = real::regex("[a-z]+"), .in_mode = {"grp"}});
    scilex::lexer lex {std::move(rules), {}, {}, scilex::error_policy::token};
    const auto    toks = lex.tokenize("(a@b)");
    // ( · a · @ (error, still in grp) · b · )
    EXPECT_EQ(toks.size(), 5U);
    EXPECT_EQ(toks[2].kind, scilex::error);
    EXPECT_EQ(toks[2].lexeme, "@"sv);
    EXPECT_EQ(toks[3].kind, 3);      // 'b' still lexed in the group mode
    EXPECT_EQ(toks[4].kind, 2);      // the ')' pops
  }

  TEST(a_pop_at_the_root_stays_fatal_under_token)
  {
    std::vector<scilex::rule> rules;
    rules.push_back({.kind = WORD, .pattern = real::regex("[a-z]+")});
    scilex::rule bad {.kind = 2, .pattern = real::regex(R"(\))")};
    bad.action = scilex::mode_action {.operation = scilex::mode_action::op::pop};
    rules.push_back(bad);
    scilex::lexer lex {std::move(rules), {}, {}, scilex::error_policy::token};
    EXPECT_THROWS(lex.tokenize("a)"), scilex::lex_error); // #2 is fatal even under recovery
  }

  TEST(a_zero_length_match_stays_fatal_under_token)
  {
    std::vector<scilex::rule> rules;
    rules.push_back({.kind = WORD, .pattern = real::regex("a*")}); // nullable: matches empty on 'b'
    scilex::lexer lex {std::move(rules), {}, {}, scilex::error_policy::token};
    EXPECT_THROWS(lex.tokenize("b"), scilex::lex_error);           // #4 is fatal even under recovery
  }

  TEST(an_error_token_flows_through_layout)
  {
    // Layout reads each token's mode; the error token inherits its mode's significance and does not
    // disturb the INDENT/DEDENT balance (nothing special to do — this pins it).
    scilex::lexer lex {word_lexer(scilex::error_policy::token)};
    const auto    flat = lex.tokenize("ab @# cd", scilex::eof_policy::append);
    const auto    laid = scilex::layout(flat, lex.mode_significant()); // no modes insignificant here
    long          depth {0};
    for (const scilex::token& tok : laid) {
      if (tok.kind == scilex::indent) {
        ++depth;
      }
      else if (tok.kind == scilex::dedent) {
        --depth;
      }
    }
    EXPECT_EQ(depth, 0L); // balanced; the error token passed through cleanly
  }
} // namespace
