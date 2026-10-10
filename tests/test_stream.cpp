// token_stream: text fed in pieces gives exactly the tokens tokenize gives the whole text -- the same
// kinds, lexemes, positions and modes, the same errors -- whatever the cuts.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <sciforge/test/framework.hpp>
#include "scilex/scilex.hpp"

#include "cpp.hpp"
#include "css.hpp"
#include "json.hpp"
#include "lisp.hpp"
#include "math.hpp"
#include "python.hpp"
#include "sql.hpp"
#include "xml.hpp"
#include "yaml.hpp"

namespace {

  //! A token as comparable text: kind, lexeme, position and mode.
  std::string describe(const scilex::token& t)
  {
    return std::to_string(t.kind) + "|" + std::string(t.lexeme) + "|" + std::to_string(t.start.offset) + ":"
           + std::to_string(t.start.line) + ":" + std::to_string(t.start.column) + "|" + std::to_string(t.mode_id);
  }

  std::vector<std::string> describe_all(const std::vector<scilex::token>& tokens)
  {
    std::vector<std::string> out;
    out.reserve(tokens.size());
    for (const scilex::token& t : tokens) {
      out.push_back(describe(t));
    }
    return out;
  }

  //! What a stream gives \p text fed in the pieces \p cuts marks (offsets, ascending); lexemes are
  //! described as they arrive, since they are valid only until the next call.
  std::vector<std::string> streamed(const scilex::lexer&            lex,
                                    std::string_view                text,
                                    const std::vector<std::size_t>& cuts)
  {
    scilex::token_stream     in   {lex.stream()};
    std::vector<std::string> out;
    std::size_t              from {0};
    for (const std::size_t cut : cuts) {
      for (const scilex::token& t : in.feed(text.substr(from, cut - from))) {
        out.push_back(describe(t));
      }
      from = cut;
    }
    for (const scilex::token& t : in.feed(text.substr(from))) {
      out.push_back(describe(t));
    }
    for (const scilex::token& t : in.finish()) {
      out.push_back(describe(t));
    }
    return out;
  }

  struct grammar
  {
    const char*                    name;
    std::function<scilex::lexer()> make;
    std::string_view               sample;
  };
} // namespace

// Every example grammar -- DFA-only, modal, layout-bearing, hybrid -- on its sample, cut in two at every
// byte (inside code points included) and in random pieces of every size up to eight bytes.
TEST(stream_equals_tokenize_for_every_cut)
{
  namespace ex = scilex::examples;
  const grammar grammars[] {
    {.name = "json", .make = ex::json::make_lexer, .sample = ex::json::sample},
    {.name = "xml", .make = ex::xml::make_lexer, .sample = ex::xml::sample},
    {.name = "yaml", .make = ex::yaml::make_lexer, .sample = ex::yaml::sample},
    {.name = "python", .make = ex::python::make_lexer, .sample = ex::python::sample},
    {.name = "python-unicode", .make = ex::python::make_lexer_unicode, .sample = ex::python::sample},
    {.name = "cpp", .make = ex::cpp::make_lexer, .sample = ex::cpp::sample},
    {.name = "css", .make = ex::css::make_lexer, .sample = ex::css::sample},
    {.name = "lisp", .make = ex::lisp::make_lexer, .sample = ex::lisp::sample},
    {.name = "math", .make = ex::math::make_lexer, .sample = ex::math::sample},
    {.name = "sql", .make = ex::sql::make_lexer, .sample = ex::sql::sample},
  };
  std::uint32_t seed     {12345};
  int           compared {0};
  for (const grammar& g : grammars) {
    const scilex::lexer            lex   {g.make()};
    const std::vector<std::string> whole {describe_all(lex.tokenize(g.sample))};
    EXPECT(!whole.empty());
    for (std::size_t cut {0}; cut <= g.sample.size(); ++cut) {
      const std::vector<std::string> got {streamed(lex, g.sample, {cut})};
      if (got != whole) {
        std::printf("stream: %s cut at %zu differs\n", g.name, cut);
      }
      EXPECT(got == whole);
      ++compared;
    }
    for (std::size_t piece {1}; piece <= 8; ++piece) {
      std::vector<std::size_t> cuts;
      for (std::size_t at {0}; at < g.sample.size();) {
        seed  = (seed * 1664525U) + 1013904223U;
        at   += 1U + ((seed >> 8U) % piece);
        if (at < g.sample.size()) {
          cuts.push_back(at);
        }
      }
      EXPECT(streamed(lex, g.sample, cuts) == whole);
      ++compared;
    }
  }
  EXPECT(compared >= 1000);
}

// Not "every token but the last": `a[^z]*z` swallows everything once the `z` arrives, so no token of the
// text before it may be returned early.
TEST(stream_waits_while_a_rule_could_still_swallow_the_tokens_before)
{
  std::vector<scilex::rule> rules;
  rules.push_back({.kind = 1, .pattern = real::regex("a[^z]*z")});
  rules.push_back({.kind = 2, .pattern = real::regex("[a-y]+")});
  rules.push_back({.kind = 3, .pattern = real::regex(" "), .skip = true});
  const scilex::lexer  lex              {std::move(rules)};
  scilex::token_stream in               {lex.stream()};
  EXPECT(in.feed("abc de").empty());
  const std::vector<scilex::token> rest {in.feed("z")};
  EXPECT_EQ(rest.size(), static_cast<std::size_t>(1));
  EXPECT_EQ(rest[0].kind, 1);
  EXPECT_EQ(rest[0].lexeme, std::string_view {"abc dez"});
  EXPECT(in.finish().empty());

  // And without the `z`, finish gives what tokenize gives.
  scilex::token_stream open {lex.stream()};
  EXPECT(open.feed("abc de").empty());
  EXPECT(describe_all(open.finish()) == describe_all(lex.tokenize("abc de")));
}

// A rule that reads the text before its token -- a lookbehind, a word boundary -- reads it in a stream as in
// tokenize: the stream keeps those bytes when it drops what it returned, whatever the cuts.
TEST(stream_keeps_the_text_a_rule_reads_before_its_token)
{
  const std::vector<scilex::rule> rules {
    {.kind = 1, .pattern = real::regex("@")},
    {.kind = 2, .pattern = real::regex(R"((?<=@@)\d+)")},
    {.kind = 3, .pattern = real::regex(R"(\b\d+)")},
    {.kind = 4, .pattern = real::regex(R"(\d+)")},
    {.kind = 5, .pattern = real::regex("[a-z]+")},
    {.kind = 6, .pattern = real::regex(R"(\s+)"), .skip = true},
  };
  const scilex::lexer            lex   {rules};
  const std::string_view         text  {"@@12 ab34 @5 x9 @@77 7"};
  const std::vector<std::string> whole {describe_all(lex.tokenize(text))};
  EXPECT_EQ(whole.size(), static_cast<std::size_t>(13));
  if (whole.size() != 13U) {
    return; // the checks below index the thirteen tokens
  }
  EXPECT(whole[2] == "2|12|2:1:3|0");   // after @@ the lookbehind rule wins
  EXPECT(whole[4] == "4|34|7:1:8|0");   // inside ab34 there is no word boundary before the digits
  EXPECT(whole[6] == "3|5|11:1:12|0");  // after a single @, \b\d+ does
  EXPECT(whole[12] == "3|7|21:1:22|0"); // and after a space
  for (std::size_t cut {0}; cut <= text.size(); ++cut) {
    EXPECT(streamed(lex, text, {cut}) == whole);
  }
  std::vector<std::size_t> bytes;
  for (std::size_t at {1}; at < text.size(); ++at) {
    bytes.push_back(at);
  }
  EXPECT(streamed(lex, text, bytes) == whole); // a byte at a time: every token's context was dropped and kept
}

// A token the next byte decides is returned at once, and the stream holds only what it has not returned.
TEST(stream_returns_decided_tokens_and_holds_only_the_rest)
{
  const scilex::lexer              lex   {scilex::examples::json::make_lexer()};
  scilex::token_stream             in    {lex.stream()};
  const std::vector<scilex::token> first {in.feed("[1, 22, 3")};
  EXPECT_EQ(first.size(), static_cast<std::size_t>(5));  // `[` `1` `,` `22` `,` -- the `3` may still grow
  EXPECT_EQ(in.buffered(), static_cast<std::size_t>(2)); // " 3": the skipped space goes back with the open munch

  std::string doc {"["};
  for (int i {0}; i < 2000; ++i) {
    doc += R"({"k": [1, 2.5, true, null, "text"]},)";
  }
  doc += "0]";
  scilex::token_stream long_in {lex.stream()};
  std::size_t          most    {0};
  std::size_t          tokens  {0};
  for (std::size_t at {0}; at < doc.size(); at += 64) {
    tokens += long_in.feed(std::string_view(doc).substr(at, 64)).size();
    most    = std::max(most, long_in.buffered());
  }
  tokens += long_in.finish().size();
  EXPECT_EQ(tokens, lex.tokenize(doc).size());
  EXPECT(most < 128); // a chunk plus the token it ends inside, never the text so far
}

// An error surfaces where tokenize reports it, in the whole text's coordinates, once the text decides it.
TEST(stream_errors_where_tokenize_does)
{
  const scilex::lexer lex      {scilex::examples::json::make_lexer()};
  const std::string   text     {"[1, 2,\n  3, @]"};
  std::size_t         expected {0};
  try {
    static_cast<void>(lex.tokenize(text));
  }
  catch (const scilex::lex_error& e) {
    expected = e.where().offset;
  }
  EXPECT_EQ(expected, text.find('@'));
  for (std::size_t cut {0}; cut <= text.size(); ++cut) {
    scilex::token_stream in  {lex.stream()};
    std::size_t          got {0};
    try {
      static_cast<void>(in.feed(text.substr(0, cut)));
      static_cast<void>(in.feed(text.substr(cut)));
      static_cast<void>(in.finish());
    }
    catch (const scilex::lex_error& e) {
      got = e.where().offset;
    }
    EXPECT_EQ(got, expected);
  }

  scilex::token_stream done {lex.stream()};
  static_cast<void>(done.finish());
  bool refused              {false};
  try {
    static_cast<void>(done.feed("1"));
  }
  catch (const std::logic_error&) {
    refused = true;
  }
  EXPECT(refused);
  bool twice {false};
  try {
    static_cast<void>(done.finish());
  }
  catch (const std::logic_error&) {
    twice = true;
  }
  EXPECT(twice);
}

// Recovery: an error run is one token, however the cuts split it -- the run may go on into text still to
// come, so it is returned only once a rule matches after it, or at the end. And it ends at the first
// position where a rule COULD match: in `@"ab 1"` it ends at the quote, which opens a string whose close
// may not have arrived yet, not at the `1` inside it that matches on its own.
// A run that reaches the end of the text is returned by finish.
TEST(stream_recovers_as_tokenize_does)
{
  const scilex::lexer lex {scilex::examples::json::make_rules(), {}, {}, scilex::error_policy::token};
  for (const std::string text : {"[1, @@#, 2, $$]", "[@\"ab 1\", 2]", "[1, $$"}) {
    const std::vector<std::string> whole {describe_all(lex.tokenize(text))};
    for (std::size_t cut {0}; cut <= text.size(); ++cut) {
      for (std::size_t second {cut}; second <= text.size(); ++second) {
        EXPECT(streamed(lex, text, {cut, second}) == whole);
      }
    }
  }
}

// An unterminated mode is reported where tokenize reports it -- where the mode was entered, even when the
// stream has dropped the text since.
TEST(stream_reports_an_unterminated_mode_where_it_was_entered)
{
  const scilex::lexer lex  {scilex::examples::xml::make_lexer()};
  const std::string   text {"<root>text <open attr"};
  std::string         expected;
  try {
    static_cast<void>(lex.tokenize(text));
  }
  catch (const scilex::lex_error& e) {
    expected = std::to_string(e.where().offset) + ":" + std::to_string(e.where().line) + ":"
               + std::to_string(e.where().column) + " " + e.what();
  }
  EXPECT(!expected.empty());
  for (std::size_t cut {0}; cut <= text.size(); ++cut) {
    scilex::token_stream in {lex.stream()};
    std::string          got;
    try {
      static_cast<void>(in.feed(text.substr(0, cut)));
      static_cast<void>(in.feed(text.substr(cut)));
      static_cast<void>(in.finish());
    }
    catch (const scilex::lex_error& e) {
      got = std::to_string(e.where().offset) + ":" + std::to_string(e.where().line) + ":"
            + std::to_string(e.where().column) + " " + e.what();
    }
    EXPECT_EQ(got, expected);
  }
}

// A zero-length win is fatal under either policy: the stream throws where tokenize does, however the
// text is cut -- `[0-9]*` wins with nothing at the `x`, once no digit can still arrive before it.
TEST(stream_refuses_a_zero_length_win_where_tokenize_does)
{
  std::vector<scilex::rule> rules;
  rules.push_back({.kind = 0, .pattern = real::regex("[0-9]*"), .skip = false});
  const scilex::lexer lex  {std::move(rules)};
  const std::string   text {"12x"};
  std::string         expected;
  try {
    static_cast<void>(lex.tokenize(text));
  }
  catch (const scilex::lex_error& e) {
    expected = std::to_string(e.where().offset) + " " + e.what();
  }
  EXPECT(!expected.empty());
  for (std::size_t cut {0}; cut <= text.size(); ++cut) {
    scilex::token_stream in {lex.stream()};
    std::string          got;
    try {
      static_cast<void>(in.feed(text.substr(0, cut)));
      static_cast<void>(in.feed(text.substr(cut)));
      static_cast<void>(in.finish());
    }
    catch (const scilex::lex_error& e) {
      got = std::to_string(e.where().offset) + " " + e.what();
    }
    EXPECT_EQ(got, expected);
  }
}

// Recovery: a mode still pushed at the end is one zero-width error token, as tokenize gives it, and only
// finish can know the text ended inside the mode.
TEST(stream_recovers_an_unterminated_mode_as_tokenize_does)
{
  const scilex::lexer            lex   {scilex::examples::xml::make_rules(), {}, {}, scilex::error_policy::token};
  const std::string              text  {"<root>text <open attr"};
  const std::vector<std::string> whole {describe_all(lex.tokenize(text))};
  EXPECT(!whole.empty());
  EXPECT(whole.back().rfind(std::to_string(scilex::error) + "||", 0) == 0); // zero-width, at the end
  for (std::size_t cut {0}; cut <= text.size(); ++cut) {
    EXPECT(streamed(lex, text, {cut}) == whole);
  }
}

// An error does not take the tokens before it: what the calls returned, then what the failing call decided
// (lex_error::decided), is every token before the error, wherever the text was cut.
TEST(stream_keeps_the_tokens_before_an_error_whatever_the_cut)
{
  const scilex::lexer lex {
    std::vector<scilex::rule> {{.kind = 0, .pattern = real::regex {"[a-z]+"}},
      {.kind                          = 1, .pattern = real::regex {"[0-9]+"}},
      {.kind                          = 2, .pattern = real::regex {" +"}, .skip = true}}};
  const std::string              text   {"abc 12 de @ x"};
  const std::vector<std::string> before {"abc", "12", "de"}; // every token before the `@`
  for (std::size_t cut {0}; cut <= text.size(); ++cut) {
    scilex::token_stream     in     {lex.stream()};
    std::vector<std::string> got;
    bool                     raised {false};
    try {
      for (const scilex::token& t : in.feed(text.substr(0, cut))) {
        got.emplace_back(t.lexeme);
      }
      for (const scilex::token& t : in.feed(text.substr(cut))) {
        got.emplace_back(t.lexeme);
      }
      for (const scilex::token& t : in.finish()) {
        got.emplace_back(t.lexeme);
      }
    }
    catch (const scilex::lex_error& e) {
      raised = true;
      for (const scilex::token& t : e.decided()) {
        got.emplace_back(t.lexeme);
      }
      EXPECT_EQ(e.where().offset, std::size_t {10}); // the `@`
    }
    EXPECT(raised);
    EXPECT(got == before);
  }
}

// A nullable rule is filed under no first byte, so the gate asks it wherever the scan stands: `(?:ab)+|x*`
// at "ab" may still grow, and cutting the text there must not split "abab".
TEST(stream_waits_on_a_nullable_rule_that_may_grow)
{
  const scilex::lexer      lex  {std::vector<scilex::rule> {{.kind = 0, .pattern = real::regex {"(?:ab)+|x*"}}}};
  const std::string        text {"ababxx"};
  std::vector<std::string> whole;
  for (const scilex::token& t : lex.tokenize(text)) {
    whole.emplace_back(t.lexeme);
  }
  EXPECT(whole == (std::vector<std::string> {"abab", "xx"}));
  for (std::size_t cut {0}; cut <= text.size(); ++cut) {
    scilex::token_stream     in {lex.stream()};
    std::vector<std::string> got;
    for (const scilex::token& t : in.feed(text.substr(0, cut))) {
      got.emplace_back(t.lexeme);
    }
    for (const scilex::token& t : in.feed(text.substr(cut))) {
      got.emplace_back(t.lexeme);
    }
    for (const scilex::token& t : in.finish()) {
      got.emplace_back(t.lexeme);
    }
    EXPECT(got == whole);
  }
}

// A mode with rules on both sides: the Pike rule `(?<=x)aa` has its match at "xaa", but the DFA's `a+b` is
// still alive there and wins "aab" once the `b` comes -- the munch stays open while the DFA's share may grow,
// whichever side holds the best match so far.
TEST(stream_waits_on_the_dfa_while_a_pike_rule_leads)
{
  const scilex::lexer lex {std::vector<scilex::rule> {{.kind = 0, .pattern = real::regex {"(?<=x)aa"}},
                             {.kind = 1, .pattern = real::regex {"a+b"}},
                             {.kind = 2, .pattern = real::regex {"x"}}}};
  const std::string        text {"xaab"};
  std::vector<std::string> whole;
  for (const scilex::token& t : lex.tokenize(text)) {
    whole.emplace_back(t.lexeme);
  }
  EXPECT(whole == (std::vector<std::string> {"x", "aab"}));
  for (std::size_t cut {0}; cut <= text.size(); ++cut) {
    scilex::token_stream     in {lex.stream()};
    std::vector<std::string> got;
    for (const scilex::token& t : in.feed(text.substr(0, cut))) {
      got.emplace_back(t.lexeme);
    }
    for (const scilex::token& t : in.feed(text.substr(cut))) {
      got.emplace_back(t.lexeme);
    }
    for (const scilex::token& t : in.finish()) {
      got.emplace_back(t.lexeme);
    }
    EXPECT(got == whole);
  }
}

namespace {
  //! The lexemes a stream gives for \p text fed in two pieces at \p cut, then finished.
  std::vector<std::string> streamed_lexemes(const scilex::lexer& lex,
                                            std::string_view     text,
                                            std::size_t          cut)
  {
    scilex::token_stream     in {lex.stream()};
    std::vector<std::string> got;
    for (const scilex::token& t : in.feed(text.substr(0, cut))) {
      got.emplace_back(t.lexeme);
    }
    for (const scilex::token& t : in.feed(text.substr(cut))) {
      got.emplace_back(t.lexeme);
    }
    for (const scilex::token& t : in.finish()) {
      got.emplace_back(t.lexeme);
    }
    return got;
  }
} // namespace

// A rule the DFA cannot take (a lookbehind) answers through can_extend: `(?<=[x@])a[^z]*z` may still swallow what
// follows "xabc de", in a mode whose other rules are on its DFA, and in an error run (`@`) that reaches the cut.
TEST(stream_waits_on_a_pike_rule_that_may_still_grow)
{
  const std::vector<scilex::rule> rules {
    {.kind = 0, .pattern = real::regex {"x"}},
    {.kind = 1, .pattern = real::regex {"(?<=[x@])a[^z]*z"}},
    {.kind = 2, .pattern = real::regex {"[b-y]+"}},
    {.kind = 3, .pattern = real::regex {" "}, .skip = true}};
  for (const scilex::error_policy policy : {scilex::error_policy::raise, scilex::error_policy::token}) {
    const scilex::lexer lex {rules, {}, {}, policy};
    for (const std::string_view text : {std::string_view {"xabc dez"}, std::string_view {"x@abc dez"}}) {
      if (policy == scilex::error_policy::raise && text.find('@') != std::string_view::npos) {
        continue; // an error run needs the token policy
      }
      std::vector<std::string> whole;
      for (const scilex::token& t : lex.tokenize(text)) {
        whole.emplace_back(t.lexeme);
      }
      EXPECT_EQ(whole.back(), std::string {text.substr(text.find('a'))}); // the lookbehind rule took the rest
      for (std::size_t cut {0}; cut <= text.size(); ++cut) {
        EXPECT(streamed_lexemes(lex, text, cut) == whole);
      }
    }
  }
}

// Each case is streamed at every cut, under both policies, with the rules on their DFAs and all on Pike (no DFA
// for the mode): a nullable lookbehind rule that may still grow (`(?<=x)b*`), an error run that ends on a byte
// a rule may start but does not match (`!a!ab`), and one that ends on a munch still open at the end (`!!aa`).
TEST(stream_matches_tokenize_with_and_without_a_dfa)
{
  struct stream_case
  {
    std::vector<scilex::rule>     rules;
    std::vector<std::string_view> texts;
  };
  const std::vector<stream_case> cases {
    {.rules   = {{.kind = 0, .pattern = real::regex {"x"}},
       {.kind = 1, .pattern = real::regex {"(?<=x)b*"}},
       {.kind = 2, .pattern = real::regex {" "}, .skip = true}},
     .texts   = {"xbb", "x xb"}},
    {.rules   = {{.kind = 0, .pattern = real::regex {"ab"}}, {.kind = 1, .pattern = real::regex {"a+"}}},
     .texts   = {"!a!ab", "!!aa", "ab!"}},
    {.rules   = {{.kind = 0, .pattern = real::regex {"ab"}}}, .texts = {"!a!ab"}}};
  for (const stream_case& c : cases) {
    for (const scilex::error_policy policy : {scilex::error_policy::raise, scilex::error_policy::token}) {
      for (const scilex::dfa_policy dfa : {scilex::dfa_policy::automatic, scilex::dfa_policy::requested}) {
        const scilex::lexer lex {c.rules, {}, {}, policy, scilex::column_unit::bytes, dfa};
        for (const std::string_view text : c.texts) {
          if (policy == scilex::error_policy::raise && text.find('!') != std::string_view::npos) {
            continue; // an error run needs the token policy
          }
          std::vector<std::string> whole;
          for (const scilex::token& t : lex.tokenize(text)) {
            whole.emplace_back(t.lexeme);
          }
          for (std::size_t cut {0}; cut <= text.size(); ++cut) {
            EXPECT(streamed_lexemes(lex, text, cut) == whole);
          }
        }
      }
    }
  }
}

// A step that pushes, sets and pops through skip rules, then waits on an open munch, leaves the mode stack as it
// found it: every cut gives the kinds and lexemes tokenize gives.
TEST(stream_undoes_the_transitions_of_a_step_that_waits)
{
  using scilex::mode_action;
  const std::vector<scilex::rule> rules {
    {.kind    = 0,
     .pattern = real::regex {"\\("},
     .skip    = true,
     .in_mode = {"default", "inner"},
     .action  = mode_action {.operation = mode_action::op::push, .target = "inner"}},
    {.kind    = 0,
     .pattern = real::regex {"\\)"},
     .skip    = true,
     .in_mode = {"inner", "digits"},
     .action  = mode_action {.operation = mode_action::op::pop}},
    {.kind    = 0,
     .pattern = real::regex {"#"},
     .skip    = true,
     .in_mode = {"inner"},
     .action  = mode_action {.operation = mode_action::op::set, .target = "digits"}},
    {.kind    = 1, .pattern = real::regex {"[a-z]+"}, .in_mode = {"default", "inner"}},
    {.kind    = 2, .pattern = real::regex {"[0-9]+"}, .in_mode = {"digits"}}};
  const scilex::lexer                      lex  {rules};
  const std::string                        text {"ab((#)(cd#12)e)fg"};
  std::vector<std::pair<int, std::string>> whole;
  for (const scilex::token& t : lex.tokenize(text)) {
    whole.emplace_back(t.kind, t.lexeme);
  }
  EXPECT_EQ(whole.size(), std::size_t {5}); // ab cd 12 e fg
  for (std::size_t cut {0}; cut <= text.size(); ++cut) {
    scilex::token_stream                     in {lex.stream()};
    std::vector<std::pair<int, std::string>> got;
    for (const std::string_view piece : {std::string_view {text}.substr(0, cut), std::string_view {text}.substr(cut)}) {
      for (const scilex::token& t : in.feed(piece)) {
        got.emplace_back(t.kind, t.lexeme);
      }
    }
    for (const scilex::token& t : in.finish()) {
      got.emplace_back(t.kind, t.lexeme);
    }
    EXPECT(got == whole);
  }
}

// A rule that takes one raw byte (`\C`) ends a token inside a code point. A column in code points or UTF-16
// units scores a lead by its whole sequence and a continuation by its lead, so the stream holds back a code
// point it has not finished and keeps its lead once returned: fed byte by byte or cut anywhere, the columns
// are tokenize's. The text holds a 4-byte and a 2-byte code point, orphan continuations and a byte no
// sequence starts with.
TEST(stream_columns_match_tokenize_inside_a_code_point)
{
  const std::vector<scilex::rule> rules {{.kind = 1, .pattern = real::regex {"\\C", real::flags::allow_raw_byte}}};
  const std::string               text  {"a\xF0\x9F\x98\x80"
                                         "b\xC3\xA9"
                                         "c\x80\x80\x80\x80\xF8"
                                         "d\xE2\x82"};
  for (const scilex::column_unit unit : {scilex::column_unit::codepoints, scilex::column_unit::utf16}) {
    const scilex::lexer      lex {rules, {}, {}, scilex::error_policy::raise, unit};
    std::vector<std::size_t> whole;
    for (const scilex::token& t : lex.tokenize(text)) {
      whole.push_back(t.start.column);
    }
    for (std::size_t cut {0}; cut <= text.size(); ++cut) {
      for (const std::size_t chunk : {std::size_t {1}, text.size()}) {
        scilex::token_stream     in {lex.stream()};
        std::vector<std::size_t> got;
        for (const std::string_view part : {std::string_view {text}.substr(0, cut), std::string_view {text}.substr(cut)}) {
          for (std::size_t at {0}; at < part.size(); at += chunk) {
            for (const scilex::token& t : in.feed(part.substr(at, chunk))) {
              got.push_back(t.start.column);
            }
          }
        }
        for (const scilex::token& t : in.finish()) {
          got.push_back(t.start.column);
        }
        EXPECT(got == whole);
      }
    }
    scilex::token_stream held {lex.stream()};
    EXPECT_EQ(held.feed("a\xF0").size(), std::size_t {1}); // `a`; the lead waits for its sequence
    EXPECT_EQ(held.buffered(), std::size_t {1});           // and counts as held
  }
  // `\x98\x80b?` may still grow when the text stops after U+1F600, so the stream waits at its 0x98, inside
  // the code point; the next feed drops what was returned but the lead, which scores 0x98 and 0x80.
  const scilex::lexer bytes_rule {{{.kind = 1, .pattern = real::regex {"\\C", real::flags::allow_raw_byte}},
    {.kind = 2, .pattern = real::regex {"\\x98\\x80b?", real::flags::bytes}}},
    {},
    {},
    scilex::error_policy::raise,
    scilex::column_unit::codepoints};
  scilex::token_stream     in {bytes_rule.stream()};
  std::vector<std::size_t> got;
  for (const std::string_view part : {std::string_view {"a\xF0\x9F\x98\x80"}, std::string_view {"bc"}}) {
    for (const scilex::token& t : in.feed(part)) {
      got.push_back(t.start.column);
    }
  }
  for (const scilex::token& t : in.finish()) {
    got.push_back(t.start.column);
  }
  EXPECT(got == (std::vector<std::size_t> {1, 2, 3, 3, 4})); // a, 0xF0, 0x9F, 0x98 0x80 b, c
}
