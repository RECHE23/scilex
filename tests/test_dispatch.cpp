// First-byte dispatch (lexer.hpp): the bucket index must never change tokenization.
// Each rule joins the bucket of its single fixed leading literal, or the general
// list (tried at every position) when it has none. These tests exercise every
// branch of that classification AND prove the general fallback is sound — a rule
// that could match a byte other than a naive "first character" must stay general,
// or its tokens would be silently dropped.
#include <string_view>
#include <vector>

#include "framework.hpp"
#include "scilex/scilex.hpp"

using namespace std::string_view_literals;

namespace {

  scilex::rule lit(int          kind,
                   const char*  pattern,
                   bool         skip = false)
  {
    return {.kind = kind, .pattern = real::regex(pattern), .skip = skip};
  }
} // namespace

TEST(literal_rules_are_bucketed_and_keep_priority)
{
  // Keywords (bucketed by first byte) and a single-char literal, before the
  // identifier class (general). On an equal-length tie the earlier rule wins —
  // the dispatch preserves rule order regardless of bucket-vs-general scan order.
  std::vector<scilex::rule> rules;
  rules.push_back({.kind = 0, .pattern = real::regex("\\s+"), .skip = true});
  rules.push_back(lit(1, "if"));     // multi-char literal -> bucket 'i'
  rules.push_back(lit(2, "int"));    // bucket 'i' too
  rules.push_back(lit(3, ";"));      // single-char literal -> bucket ';'
  rules.push_back({.kind = 4, .pattern = real::regex("[a-z]+")});
  const scilex::lexer lexer(std::move(rules));

  const auto tokens = lexer.tokenize("if int ix;");
  EXPECT_EQ(tokens.size(), 4U);
  EXPECT_EQ(tokens[0].kind, 1);        // "if"  -> keyword (tie vs [a-z]+, earlier wins)
  EXPECT_EQ(tokens[1].kind, 2);        // "int" -> keyword
  EXPECT_EQ(tokens[2].kind, 4);        // "ix"  -> identifier (no keyword "ix")
  EXPECT_EQ(tokens[2].lexeme, "ix"sv);
  EXPECT_EQ(tokens[3].kind, 3);        // ";"   -> single-char literal
}

TEST(case_insensitive_literal_falls_back_to_general)
{
  // An icase literal can match either case, so it must NOT be pinned to one byte:
  // bucketed under 'i', it would be skipped at the 'I' position and "IF" would lex
  // as the identifier instead. In the general list it is tried everywhere.
  std::vector<scilex::rule> rules;
  rules.push_back({.kind = 1, .pattern = real::regex("if", real::flags::icase), .skip = false});
  rules.push_back({.kind = 2, .pattern = real::regex("[A-Za-z]+")});
  const scilex::lexer lexer(std::move(rules));

  EXPECT_EQ(lexer.tokenize("IF").at(0).kind, 1); // matched at 'I' -> keyword, not identifier
  EXPECT_EQ(lexer.tokenize("if").at(0).kind, 1);
}

TEST(empty_pattern_rule_is_general_and_inert)
{
  // An empty pattern matches the empty string at every position: no fixed first
  // byte, so it goes to general, and (empty matches being ignored) it never emits.
  std::vector<scilex::rule> rules;
  rules.push_back({.kind = 1, .pattern = real::regex(""), .skip = false});
  rules.push_back({.kind = 2, .pattern = real::regex("[0-9]+")});
  const scilex::lexer lexer(std::move(rules));

  const auto tokens = lexer.tokenize("42");
  EXPECT_EQ(tokens.size(), 1U);
  EXPECT_EQ(tokens[0].kind, 2);
}

TEST(optional_leading_literal_falls_back_to_general)
{
  // "a?b" can start with 'a' OR 'b' (the 'a' is optional), so it must not be bucketed
  // under 'a'; in general it correctly matches "b" (where the naive bucket would skip
  // it) as well as "ab".
  std::vector<scilex::rule> rules;
  rules.push_back({.kind = 1, .pattern = real::regex("a?b"), .skip = false});
  rules.push_back({.kind = 2, .pattern = real::regex("[a-z]")});
  const scilex::lexer lexer(std::move(rules));

  EXPECT_EQ(lexer.tokenize("b").at(0).lexeme, "b"sv);   // matched at 'b' -> kind 1
  EXPECT_EQ(lexer.tokenize("b").at(0).kind, 1);
  EXPECT_EQ(lexer.tokenize("ab").at(0).lexeme, "ab"sv); // matched at 'a' -> kind 1
}

TEST(alternation_rule_falls_back_to_general)
{
  // "cat|dog" can start with 'c' or 'd', so it must not be bucketed under 'c'; in
  // general it matches both branches.
  std::vector<scilex::rule> rules;
  rules.push_back({.kind = 0, .pattern = real::regex("\\s+"), .skip = true});
  rules.push_back({.kind = 1, .pattern = real::regex("cat|dog"), .skip = false});
  rules.push_back({.kind = 2, .pattern = real::regex("[a-z]+")});
  const scilex::lexer lexer(std::move(rules));

  EXPECT_EQ(lexer.tokenize("cat").at(0).kind, 1);
  EXPECT_EQ(lexer.tokenize("dog").at(0).kind, 1); // matched at 'd' -> the second branch
}

TEST(tie_break_favours_the_earlier_rule_across_bucket_and_general)
{
  // A general (class) rule placed *before* a bucketed literal: on an equal-length tie
  // the earlier rule must still win, even though the bucketed literal is tried first.
  std::vector<scilex::rule> rules;
  rules.push_back({.kind = 1, .pattern = real::regex("[a-z]+")}); // general, earlier in rules_
  rules.push_back(lit(2, "if"));                                  // bucket 'i', later in rules_
  const scilex::lexer lexer(std::move(rules));

  EXPECT_EQ(lexer.tokenize("if").at(0).kind, 1);                  // "[a-z]+" wins the len-2 tie (earlier rule)
}

TEST(dispatch_matches_a_mixed_stream)
{
  // A broader stream mixing bucketed literals, classes, and skipped trivia — the
  // full token sequence must be exactly what the maximal-munch rules dictate.
  std::vector<scilex::rule> rules;
  rules.push_back({.kind = 0, .pattern = real::regex("\\s+"), .skip = true});
  rules.push_back(lit(1, "let"));
  rules.push_back(lit(2, "="));
  rules.push_back({.kind = 3, .pattern = real::regex("[A-Za-z_]\\w*")});
  rules.push_back({.kind = 4, .pattern = real::regex("[0-9]+")});
  const scilex::lexer lexer(std::move(rules));

  const auto tokens = lexer.tokenize("let total = 42");
  EXPECT_EQ(tokens.size(), 4U);
  EXPECT_EQ(tokens[0].kind, 1);          // "let"   -> keyword
  EXPECT_EQ(tokens[1].kind, 3);          // "total" -> identifier (not the "let" bucket)
  EXPECT_EQ(tokens[1].lexeme, "total"sv);
  EXPECT_EQ(tokens[2].kind, 2);          // "="     -> literal
  EXPECT_EQ(tokens[3].kind, 4);          // "42"    -> number
}
