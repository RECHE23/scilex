// A rule that depends on the token before it (rule::after, rule::not_after): a `/` after an operand is a
// division and anywhere else opens a regular expression literal, as JavaScript lexes it; the previous token
// is the last one emitted in any mode, a skipped match is not one and an error run is; every scan (whole
// text, lazy range, stream fed a byte at a time) gives the same tokens; the modes stay on their DFA.
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sciforge/test/framework.hpp>
#include "scilex/scilex.hpp"

using namespace std::string_view_literals;

namespace {

  // Token kinds (plain ints, as a rule's kind is).
  constexpr int ws       {0};
  constexpr int ident    {1};
  constexpr int number   {2};
  constexpr int lparen   {3};
  constexpr int rparen   {4};
  constexpr int op       {5};
  constexpr int division {6};
  constexpr int regex    {7};

  scilex::rule plain(int         k,
                     const char* pattern,
                     bool        skip = false)
  {
    return scilex::rule {.kind = k, .pattern = real::regex(pattern), .skip = skip};
  }

  // An operand ends in an identifier, a number or a closing parenthesis: a `/` after one divides.
  std::vector<scilex::rule> javascript_like()
  {
    const std::vector<int>    operands {ident, number, rparen};
    std::vector<scilex::rule> rules;
    rules.push_back(plain(ws, R"(\s+)", true));
    rules.push_back(plain(ident, R"((?a)[A-Za-z_]\w*)")); // ASCII: a Unicode \w would stay on Pike
    rules.push_back(plain(number, R"([0-9]+)"));
    rules.push_back(plain(lparen, R"(\()"));
    rules.push_back(plain(rparen, R"(\))"));
    rules.push_back(plain(op, R"([=+.*-])"));
    scilex::rule div {plain(division, "/")};
    div.after = operands;
    rules.push_back(std::move(div));
    scilex::rule re {plain(regex, R"(/(?:[^/\\\n]|\\.)+/[a-z]*)")};
    re.not_after = operands;
    rules.push_back(std::move(re));
    return rules;
  }

  std::vector<std::pair<int, std::string>> pairs(const std::vector<scilex::token>& tokens)
  {
    std::vector<std::pair<int, std::string>> out;
    out.reserve(tokens.size());
    for (const scilex::token& t : tokens) {
      out.emplace_back(t.kind, std::string {t.lexeme});
    }
    return out;
  }

  using expected = std::vector<std::pair<int, std::string>>;

  // tokenize, scan() and a stream fed one byte at a time.
  bool every_scan_agrees(const scilex::lexer& lex,
                         std::string_view     text)
  {
    const std::vector<scilex::token> whole {lex.tokenize(text)};
    std::vector<scilex::token>       lazy;
    for (const scilex::token& t : lex.scan(text)) {
      lazy.push_back(t);
    }
    std::vector<std::pair<int, std::string>> streamed;
    scilex::token_stream                     in {lex.stream()};
    for (std::size_t i {0}; i < text.size(); ++i) {
      for (const scilex::token& t : in.feed(text.substr(i, 1))) {
        streamed.emplace_back(t.kind, std::string {t.lexeme});
      }
    }
    for (const scilex::token& t : in.finish()) {
      streamed.emplace_back(t.kind, std::string {t.lexeme});
    }
    return pairs(lazy) == pairs(whole) && streamed == pairs(whole);
  }
} // namespace

TEST(a_slash_divides_after_an_operand_and_opens_a_regex_elsewhere)
{
  const scilex::lexer lex {javascript_like()};
  EXPECT(pairs(lex.tokenize("a / b / c")) == (expected {{ident, "a"}, {division, "/"}, {ident, "b"}, {division, "/"}, {ident, "c"}}));
  EXPECT(pairs(lex.tokenize("x = /ab+c/g.test")) == (expected {{ident, "x"}, {op, "="}, {regex, "/ab+c/g"}, {op, "."}, {ident, "test"}}));
  EXPECT(pairs(lex.tokenize("(a)/2")) == (expected {{lparen, "("}, {ident, "a"}, {rparen, ")"}, {division, "/"}, {number, "2"}}));
  EXPECT(pairs(lex.tokenize("/x/")) == (expected {{regex, "/x/"}}));                                    // before the first token: no operand
  EXPECT(pairs(lex.tokenize("a   /   b")) == (expected {{ident, "a"}, {division, "/"}, {ident, "b"}})); // a skip is no token
  for (const std::string_view text : {"a / b / c"sv, "x = /ab+c/g.test"sv, "(a)/2 + /re/ * 3/4"sv, "/x/"sv, ""sv}) {
    EXPECT(every_scan_agrees(lex, text));
  }
}

TEST(each_variant_of_the_mode_stays_on_the_dfa)
{
  const scilex::lexer lex {javascript_like()};
  EXPECT(lex.dfa_modes_active() == (std::vector<std::string> {"default"}));
  EXPECT(lex.pike_rules("default").empty());
}

TEST(after_may_name_the_start_of_the_input)
{
  std::vector<scilex::rule> rules   {plain(ws, R"(\s+)", true), plain(ident, R"([a-z]+)")};
  scilex::rule              shebang {plain(regex, R"(#![^\n]*)")};
  shebang.after = {scilex::end_of_input}; // only as the very first token
  rules.push_back(std::move(shebang));
  rules.push_back(plain(op, "#"));
  const scilex::lexer lex {rules};
  EXPECT(pairs(lex.tokenize("#!/bin/sh x")) == (expected {{regex, "#!/bin/sh x"}}));
  EXPECT(pairs(lex.tokenize("x #")) == (expected {{ident, "x"}, {op, "#"}})); // after a token, `#` alone
}

TEST(an_error_run_is_the_previous_token)
{
  std::vector<scilex::rule> rules {javascript_like()};
  const scilex::lexer       lex   {rules, {}, {}, scilex::error_policy::token};
  // `@` lexes as nothing: an error run, which is not an operand, so the `/` after it opens a regex.
  EXPECT(pairs(lex.tokenize("a@/b/")) == (expected {{ident, "a"}, {scilex::error, "@"}, {regex, "/b/"}}));
  EXPECT(every_scan_agrees(lex, "a@/b/ c / d"));
}

TEST(a_token_of_another_mode_is_the_previous_token)
{
  // A string is an operand: `"x" / 2` divides, though the closing quote is lexed in the string's mode.
  std::vector<scilex::rule> rules {javascript_like()};
  const int                 quote {regex + 1};
  const int                 text  {regex + 2};
  rules.push_back(scilex::rule {.kind = quote, .pattern = real::regex("\""), .action = scilex::mode_action {.operation = scilex::mode_action::op::push, .target = "str"}});
  scilex::rule body               {plain(text, R"([^"]+)")};
  body.in_mode = {"str"};
  rules.push_back(std::move(body));
  scilex::rule close {plain(quote, "\"")};
  close.in_mode = {"str"};
  close.action  = scilex::mode_action {.operation = scilex::mode_action::op::pop};
  rules.push_back(std::move(close));
  for (scilex::rule& r : rules) {
    if (r.kind == division) {
      r.after.push_back(quote);
    }
    if (r.kind == regex) {
      r.not_after.push_back(quote);
    }
  }
  const scilex::lexer lex {rules};
  EXPECT(pairs(lex.tokenize(R"("x" / 2)")) == (expected {{quote, "\""}, {text, "x"}, {quote, "\""}, {division, "/"}, {number, "2"}}));
  EXPECT(every_scan_agrees(lex, R"(a = "x" / 2 + /y/)"));
}

TEST(a_rule_takes_after_or_not_after_not_both)
{
  scilex::rule both {plain(op, "/")};
  both.after     = {ident};
  both.not_after = {number};
  EXPECT_THROWS((void)scilex::lexer({plain(ident, "[a-z]+"), both}), std::invalid_argument);
}
