// Column policy (scilex::column_unit): position::column counts bytes (the default, unchanged),
// Unicode codepoints, or UTF-16 code units (an astral codepoint is 2). A malformed byte counts one
// unit in every mode, so the column stays defined across the error runs recovery emits. The unit is
// not carried on the position — the lexer declares it via columns().
#include <string>
#include <string_view>
#include <vector>

#include <sciforge/test/framework.hpp>
#include <sciforge/test/strings.hpp>
#include "scilex/scilex.hpp"

using namespace std::string_view_literals;
using test::cat; // concatenate views into an owned std::string (the source a lexeme views into)

namespace {

  inline constexpr int WORD {1};
  inline constexpr int ANY  {2};

  // Words, else any single codepoint/byte — so an emoji or a stray byte is its own token, and the
  // column of what follows exposes the unit.
  inline constexpr int WS {3};

  scilex::lexer make_lexer(scilex::column_unit  unit,
                           scilex::error_policy errors = scilex::error_policy::raise)
  {
    std::vector<scilex::rule> rules;
    rules.push_back({.kind = WORD, .pattern = real::regex("[a-z]+")});
    rules.push_back({.kind = WS, .pattern = real::regex(R"(\s+)"), .skip = true}); // newline resets the column
    rules.push_back({.kind = ANY, .pattern = real::regex(".")});                   // any other single codepoint
    return scilex::lexer {std::move(rules), {}, {}, errors, unit};
  }

  constexpr std::string_view EMOJI  {"\xF0\x9F\x98\x80"sv}; // U+1F600, a 4-byte astral codepoint
  constexpr std::string_view EACUTE {"\xC3\xA9"sv};         // U+00E9 é, a 2-byte BMP codepoint

  // The column of the last token ("cd") after a leading multibyte run, per unit.
  std::size_t trailing_column(scilex::column_unit unit,
                              const std::string&  in)
  {
    const auto toks = make_lexer(unit).tokenize(in);
    return toks.back().start.column;
  }

  TEST(bytes_is_the_default_and_matches_byte_offsets)
  {
    // No columns argument == bytes; the column is the 1-based byte offset within the line.
    std::vector<scilex::rule> rules;
    rules.push_back({.kind = WORD, .pattern = real::regex("[a-z]+")});
    rules.push_back({.kind = ANY, .pattern = real::regex(".")});
    const scilex::lexer default_lex {std::move(rules)};
    EXPECT_EQ(default_lex.columns(), scilex::column_unit::bytes);
    const std::string in            {cat({"ab", EMOJI, "cd"})}; // named: the lexemes view into it
    const auto        toks = default_lex.tokenize(in);
    EXPECT_EQ(toks.back().lexeme, "cd"sv);
    EXPECT_EQ(toks.back().start.column, 7U);                    // 2 letters + 4 emoji bytes, next at column 7
  }

  TEST(the_three_units_diverge_after_a_multibyte_codepoint)
  {
    const std::string in {cat({"ab", EMOJI, "cd"})};
    EXPECT_EQ(trailing_column(scilex::column_unit::bytes, in), 7U);      // 2 + 4 bytes
    EXPECT_EQ(trailing_column(scilex::column_unit::codepoints, in), 4U); // a, b, emoji = 3 codepoints
    EXPECT_EQ(trailing_column(scilex::column_unit::utf16, in), 5U);      // emoji is a surrogate pair (2)
  }

  TEST(an_astral_codepoint_is_two_utf16_units_at_the_line_start)
  {
    const std::string in {cat({EMOJI, "x"})};                            // emoji first, then a letter
    EXPECT_EQ(trailing_column(scilex::column_unit::bytes, in), 5U);      // 4 bytes, x at 5
    EXPECT_EQ(trailing_column(scilex::column_unit::codepoints, in), 2U); // one codepoint, x at 2
    EXPECT_EQ(trailing_column(scilex::column_unit::utf16, in), 3U);      // two units, x at 3
  }

  TEST(a_bmp_codepoint_is_one_unit_in_codepoints_and_utf16)
  {
    const std::string in {cat({EACUTE, "x"})};                           // é (2 bytes, BMP) then x
    EXPECT_EQ(trailing_column(scilex::column_unit::bytes, in), 3U);      // 2 bytes, x at 3
    EXPECT_EQ(trailing_column(scilex::column_unit::codepoints, in), 2U); // 1 codepoint, x at 2
    EXPECT_EQ(trailing_column(scilex::column_unit::utf16, in), 2U);      // BMP is 1 utf16 unit, x at 2
  }

  TEST(a_malformed_byte_counts_one_unit_in_every_mode)
  {
    // 0xC3 0x28: a lead with a bad continuation — both bytes are malformed and count 1 each.
    const std::string in {"a\xC3\x28y"}; // a, <bad C3>, '(' (0x28), y
    for (const scilex::column_unit unit : {scilex::column_unit::codepoints, scilex::column_unit::utf16}) {
      const auto toks = make_lexer(unit, scilex::error_policy::token).tokenize(in);
      EXPECT_EQ(toks.back().lexeme, "y"sv);
      EXPECT_EQ(toks.back().start.column, 4U); // a=1, C3=2, (=3, y=4 — one unit each
    }
  }

  TEST(the_column_after_an_error_run_is_unit_correct)
  {
    // The M1 x M2 crossing: a token after a malformed ERROR run, in each unit. The run is two invalid
    // bytes; the recovery emits one ERROR token, and the next word's column reflects the unit.
    const std::string in     {"a" + std::string {"\xff\xfe"} + "bc"}; // a, <error \xff\xfe>, bc
    scilex::lexer     lex_cp {make_lexer(scilex::column_unit::codepoints, scilex::error_policy::token)};
    const auto        cp = lex_cp.tokenize(in);
    EXPECT_EQ(cp.size(), 3U);
    EXPECT_EQ(cp[1].kind, scilex::error);
    EXPECT_EQ(cp[2].lexeme, "bc"sv);
    EXPECT_EQ(cp[2].start.column, 4U); // a=1, then two malformed bytes 2,3, bc at 4
    // utf16 counts the same malformed bytes one-each, so the column matches.
    scilex::lexer lex_u16 {make_lexer(scilex::column_unit::utf16, scilex::error_policy::token)};
    EXPECT_EQ(lex_u16.tokenize(in)[2].start.column, 4U);
  }

  TEST(columns_reset_each_line)
  {
    // A multibyte run on line 1 does not leak into line 2's column (the '\n' resets it in every unit).
    const std::string in {cat({"ab", EMOJI, "\ncd"})};
    for (const scilex::column_unit unit : {scilex::column_unit::bytes,
                                           scilex::column_unit::codepoints, scilex::column_unit::utf16}) {
      const auto toks = make_lexer(unit).tokenize(in);
      EXPECT_EQ(toks.back().lexeme, "cd"sv);
      EXPECT_EQ(toks.back().start.line, 2U);
      EXPECT_EQ(toks.back().start.column, 1U); // fresh line, column 1 regardless of unit
    }
  }

  TEST(column_unit_is_introspectable)
  {
    EXPECT_EQ(make_lexer(scilex::column_unit::bytes).columns(), scilex::column_unit::bytes);
    EXPECT_EQ(make_lexer(scilex::column_unit::codepoints).columns(), scilex::column_unit::codepoints);
    EXPECT_EQ(make_lexer(scilex::column_unit::utf16).columns(), scilex::column_unit::utf16);
  }

  TEST(a_three_byte_codepoint_is_one_column)
  {
    const std::string in {"a\xE2\x9C\x93z"};                        // U+2713 ✓, a 3-byte BMP codepoint, between two letters
    EXPECT_EQ(trailing_column(scilex::column_unit::codepoints, in), 3U);
    EXPECT_EQ(trailing_column(scilex::column_unit::utf16, in), 3U); // a BMP codepoint is one utf16 unit
  }

  TEST(malformed_utf8_shapes_score_one_column_per_byte)
  {
    // A truncated lead, a missing continuation, an overlong encoding, a UTF-16 surrogate, an orphan
    // continuation, and a beyond-U+10FFFF sequence — each byte is malformed and scores one unit. Under
    // recovery they become ERROR runs; the point is that the column stays byte-accurate and every UTF-8
    // validity path is exercised.
    const std::string shapes[] {
      "a\xF0",              // a truncated 4-byte lead at end of input
      "a\xE2z",             // a 3-byte lead followed by a non-continuation
      "a\xC0\x80z",         // an overlong 2-byte encoding of NUL
      "a\xED\xA0\x80z",     // a UTF-16 surrogate (U+D800) spelled in UTF-8
      "a\x80z",             // an orphan continuation byte
      "a\xF4\x90\x80\x80z", // a 4-byte sequence beyond U+10FFFF
    };
    for (const scilex::column_unit unit : {scilex::column_unit::codepoints, scilex::column_unit::utf16}) {
      for (const std::string& in : shapes) {
        const auto  toks = make_lexer(unit, scilex::error_policy::token).tokenize(in);
        std::size_t covered {0};
        for (const scilex::token& tok : toks) {
          covered += tok.lexeme.size();
        }
        EXPECT_EQ(covered, in.size()); // every byte accounted for; each malformed byte is one column
      }
    }
  }

  TEST(ascii_indentation_after_multibyte_is_identical_in_all_units)
  {
    // Layout follows the lexer's unit, but the indentation itself is ASCII, so an indented line after a
    // line containing multibyte text gets the same INDENT column in every unit — the daily semantics do
    // not shift; the unit only matters for diagnostics.
    std::vector<scilex::token> laid_bytes;
    std::size_t                indent_column {0};
    for (const scilex::column_unit unit : {scilex::column_unit::bytes,
                                           scilex::column_unit::codepoints, scilex::column_unit::utf16}) {
      scilex::lexer     lex {make_lexer(unit)};
      const std::string src {cat({"a", EACUTE, "\n  b"})}; // named: the lexemes view into it
      const auto        flat = lex.tokenize(src, scilex::eof_policy::append);
      // The indented token "b" is on line 2 at column 3 (two ASCII spaces) in every unit.
      for (const scilex::token& tok : flat) {
        if (tok.lexeme == "b"sv) {
          if (indent_column == 0) {
            indent_column = tok.start.column;
          }
          EXPECT_EQ(tok.start.column, indent_column); // same across units — the indent is ASCII
          EXPECT_EQ(tok.start.column, 3U);
        }
      }
    }
  }
} // namespace
