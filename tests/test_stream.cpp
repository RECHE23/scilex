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
