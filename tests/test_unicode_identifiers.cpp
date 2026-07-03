// Unicode identifiers vs DFA speed — the grammar author's choice, pinned as tested behaviour.
// A `\w`-based rule with the default flags reads Unicode identifiers (café, 変数) but is a match-time
// code-point predicate, so a mode holding it leaves the DFA fast path; pinning `(?a)` keeps it ASCII
// and DFA-representable. The token stream is identical on ASCII input; it differs only on non-ASCII.
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <sciforge/test/framework.hpp>
#include "scilex/scilex.hpp"

using namespace std::string_view_literals;

namespace {

  inline constexpr int WORD {1};
  inline constexpr int WS   {2};

  // A one-mode grammar whose identifier rule is either ASCII-pinned or Unicode.
  scilex::lexer word_lexer(std::string_view                 word_pattern,
                           std::unordered_set<std::string>  dfa     = {},
                           scilex::error_policy             errors  = scilex::error_policy::raise)
  {
    std::vector<scilex::rule> rules;
    rules.push_back({.kind = WORD, .pattern = real::regex(std::string {word_pattern})});
    rules.push_back({.kind = WS, .pattern = real::regex(R"((?a)\s+)"), .skip = true});
    return scilex::lexer {std::move(rules), {}, std::move(dfa), errors};
  }

  constexpr std::string_view CAFE   {"caf\xC3\xA9"sv};              // café (U+00E9 é is a 2-byte word char)
  constexpr std::string_view HENSU  {"\xE5\xA4\x89\xE6\x95\xB0"sv}; // 変数 (two 3-byte CJK word chars)
  constexpr std::string_view ASTRAL {"\xF0\xA0\x80\x80"sv};         // U+20000, a 4-byte CJK-Ext-B word char

  TEST(ascii_pinned_word_stays_ascii)
  {
    // (?a)\w+ is ASCII: on café it matches "caf" and then cannot lex the é — one word, not "café".
    scilex::lexer     lex       {word_lexer(R"((?a)\w+)")};
    bool              threw     {false};
    std::size_t       first_len {0};
    try {
      const auto toks = lex.tokenize(CAFE);
      first_len = toks.empty() ? 0 : toks[0].lexeme.size();
    }
    catch (const scilex::lex_error&) {
      threw = true;
    }
    EXPECT_EQ(threw, true);      // the non-ASCII é is unlexable under the ASCII pin
    EXPECT_EQ(first_len, 0U);    // (nothing returned — the throw aborts the whole tokenize)
  }

  TEST(text_word_recognizes_a_unicode_identifier)
  {
    scilex::lexer     lex {word_lexer(R"(\w+)")};
    const auto        cafe = lex.tokenize(CAFE);
    EXPECT_EQ(cafe.size(), 1U);          // café is one identifier
    EXPECT_EQ(cafe[0].kind, WORD);
    EXPECT_EQ(cafe[0].lexeme, CAFE);     // the whole 5-byte lexeme
    const auto hensu = lex.tokenize(HENSU);
    EXPECT_EQ(hensu.size(), 1U);         // 変数 is one identifier
  }

  TEST(text_word_recognizes_a_four_byte_codepoint)
  {
    scilex::lexer lex {word_lexer(R"(\w+)")};
    const auto    toks = lex.tokenize(ASTRAL);
    EXPECT_EQ(toks.size(), 1U);          // a 4-byte astral letter is one identifier
    EXPECT_EQ(toks[0].lexeme, ASTRAL);
  }

  TEST(the_same_ascii_input_tokenizes_identically_either_way)
  {
    // The two spellings differ only on non-ASCII input; on ASCII they are the same.
    const std::string in {"abc def"};
    const auto        pinned = word_lexer(R"((?a)\w+)").tokenize(in);
    const auto        text   = word_lexer(R"(\w+)").tokenize(in);
    EXPECT_EQ(pinned.size(), text.size());
    for (std::size_t i {0}; i < pinned.size(); ++i) {
      EXPECT_EQ(pinned[i].lexeme, text[i].lexeme);
    }
  }

  TEST(a_unicode_shorthand_demotes_the_dfa)
  {
    // The trade-off as tested behaviour: the SAME mode is DFA-accelerated when its word rule is
    // ASCII-pinned, and demoted (absent from dfa_modes_active) when it is a Unicode code-point predicate.
    const scilex::lexer pinned        {word_lexer(R"((?a)\w+)", {"default"})};
    const scilex::lexer unicode       {word_lexer(R"(\w+)", {"default"})};
    bool                pinned_active {false};
    for (const std::string& mode : pinned.dfa_modes_active()) {
      pinned_active = pinned_active || mode == "default";
    }
    bool unicode_active {false};
    for (const std::string& mode : unicode.dfa_modes_active()) {
      unicode_active = unicode_active || mode == "default";
    }
    EXPECT_EQ(pinned_active, true);   // (?a)\w+ is DFA-representable — accelerated
    EXPECT_EQ(unicode_active, false); // \w+ is a code-point predicate — demoted to Pike
  }
} // namespace
