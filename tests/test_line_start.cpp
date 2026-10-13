// A rule kept to the start of a line, or to past it (rule::at_line_start): a `#` opens a heading at the start of
// a line and is a word past it; the start of the text is one, an indented line's first token is not; every scan
// (whole text, lazy range, stream fed a byte at a time) gives the same tokens; the modes stay on their DFA.
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sciforge/test/framework.hpp>
#include "scilex/scilex.hpp"

using namespace std::string_view_literals;

namespace {

  constexpr int ws      {0};
  constexpr int heading {1};
  constexpr int word    {2};
  constexpr int newline {3};
  constexpr int marker  {4};

  scilex::rule plain(int         k,
                     const char* pattern,
                     bool        skip = false)
  {
    return scilex::rule {.kind = k, .pattern = real::regex(pattern), .skip = skip};
  }

  scilex::rule placed(scilex::rule r,
                      bool         line_start)
  {
    r.at_line_start = line_start;
    return r;
  }

  // Markdown's `#`: a heading at the start of a line, part of a word past it.
  std::vector<scilex::rule> markdown_like()
  {
    return {plain(ws, R"([ \t]+)", true), plain(newline, R"(\n)"), placed(plain(heading, R"(#[^\n]*)"), true),
            plain(word, R"([^\s]+)")};
  }

  using expected = std::vector<std::pair<int, std::string>>;

  expected pairs(const std::vector<scilex::token>& tokens)
  {
    expected out;
    for (const scilex::token& t : tokens) {
      out.emplace_back(t.kind, std::string {t.lexeme});
    }
    return out;
  }

  // tokenize, scan() and a stream fed one byte at a time.
  bool every_scan_agrees(const scilex::lexer& lex,
                         std::string_view     text)
  {
    const std::vector<scilex::token> whole {lex.tokenize(text)};
    std::vector<scilex::token>       lazy;
    for (const scilex::token& t : lex.scan(text)) {
      lazy.push_back(t);
    }
    expected             streamed;
    scilex::token_stream in {lex.stream()};
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

TEST(a_hash_opens_a_heading_only_at_the_start_of_a_line)
{
  const scilex::lexer lex {markdown_like()};
  EXPECT(pairs(lex.tokenize("# Title\nsee #3\n"))
         == (expected {{heading, "# Title"}, {newline, "\n"}, {word, "see"}, {word, "#3"}, {newline, "\n"}}));
  EXPECT(pairs(lex.tokenize("  # no")) == (expected {{word, "#"}, {word, "no"}})); // past the indentation
  EXPECT(pairs(lex.tokenize("a\n#b")) == (expected {{word, "a"}, {newline, "\n"}, {heading, "#b"}}));
  for (const std::string_view text : {"# Title\nsee #3\n"sv, "  # no\n# yes"sv, "a #\n#\n\n# c #"sv, ""sv}) {
    EXPECT(every_scan_agrees(lex, text));
  }
}

TEST(a_rule_kept_past_the_start_of_a_line_and_one_kept_to_it_split_a_pattern)
{
  // YAML's `---`: a document marker at the start of a line, a word past it.
  const scilex::lexer lex {{plain(ws, R"(\s+)", true), placed(plain(marker, "---"), true), placed(plain(word, R"(-+)"), false)}};
  EXPECT(pairs(lex.tokenize("---\n --- ---")) == (expected {{marker, "---"}, {word, "---"}, {word, "---"}}));
  EXPECT(every_scan_agrees(lex, "---\n --- ---\n---"));
}

TEST(a_stream_reads_the_byte_before_a_token_it_has_dropped_the_text_of)
{
  // `#b` follows the word `a` with no blank: a stream that dropped `a` must still see that `#b` is past the start.
  const scilex::lexer lex {{plain(newline, R"(\n)"), placed(plain(heading, R"(#[a-z]*)"), true),
                            placed(plain(marker, R"(#[a-z]*)"), false), plain(word, R"([a-z]+)")}};
  EXPECT(pairs(lex.tokenize("a#b\n#c")) == (expected {{word, "a"}, {marker, "#b"}, {newline, "\n"}, {heading, "#c"}}));
  EXPECT(every_scan_agrees(lex, "a#b\n#c"));
}

TEST(each_place_on_the_line_stays_on_the_dfa)
{
  const scilex::lexer lex {markdown_like()};
  EXPECT(lex.dfa_modes_active() == (std::vector<std::string> {"default"}));
  EXPECT(lex.pike_rules("default").empty());
}

TEST(the_start_of_a_line_combines_with_the_token_before)
{
  // A heading only at the start of a line that follows a blank line: after a newline token, at a line's start.
  std::vector<scilex::rule> rules {markdown_like()};
  rules[2].after = {newline, scilex::end_of_input};
  const scilex::lexer lex         {rules};
  EXPECT(pairs(lex.tokenize("#a\n#b c #d")) == (expected {{heading, "#a"}, {newline, "\n"}, {heading, "#b c #d"}}));
  EXPECT(pairs(lex.tokenize("x #a")) == (expected {{word, "x"}, {word, "#a"}})); // after a word, past the start
  EXPECT(every_scan_agrees(lex, "#a\n#b c #d\nx #e\n#f"));
}

TEST(an_error_run_ends_where_a_rule_of_the_line_start_matches)
{
  // No rule lexes `@` or a line break: the run from `@` crosses the line break and stops at the heading, which
  // only the start of the line lets begin.
  const scilex::lexer lex {{placed(plain(heading, R"(#[a-z]*)"), true), plain(word, R"([a-z]+)")}, {}, {}, scilex::error_policy::token};
  EXPECT(pairs(lex.tokenize("x@#\n#b")) == (expected {{word, "x"}, {scilex::error, "@#\n"}, {heading, "#b"}}));
  EXPECT(every_scan_agrees(lex, "x@#\n#b@\n"));
}
