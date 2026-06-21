// First-byte dispatch (lexer.hpp): the bucket index must never change tokenization.
// Each rule is classified by REAL's exact first-byte API — a single possible first
// byte joins that one bucket; a class / alternation / icase literal joins every
// bucket its set admits; a nullable pattern (which can match the empty string) joins
// the general list and is tried at every position. These tests pin that the dispatch
// is exact (it tries a rule iff the rule can begin there) and that it never changes
// the maximal-munch result, whatever bucket-vs-general scan order applies.
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
  // identifier class. On an equal-length tie the earlier rule wins — the dispatch
  // preserves rule order regardless of bucket-vs-general scan order.
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

TEST(case_insensitive_literal_buckets_both_cases)
{
  // An icase literal folds to several first bytes; REAL's exact set is {i, I}, so it
  // is bucketed under BOTH — tried whether the input is 'if' or 'IF'. (The old
  // textual heuristic gave up on any flagged rule and dumped it in the general list;
  // exact dispatch buckets it instead — the win behind SQL's icase keywords.)
  std::vector<scilex::rule> rules;
  rules.push_back({.kind = 1, .pattern = real::regex("if", real::flags::icase), .skip = false});
  rules.push_back({.kind = 2, .pattern = real::regex("[A-Za-z]+")});
  const scilex::lexer lexer(std::move(rules));

  EXPECT_EQ(lexer.tokenize("IF").at(0).kind, 1); // matched at 'I' -> keyword, not identifier
  EXPECT_EQ(lexer.tokenize("if").at(0).kind, 1);
}

TEST(empty_pattern_rule_is_general_and_inert)
{
  // An empty pattern can match the empty string at every position: no usable
  // first-byte set, so it goes to the general list, and (empty matches being
  // ignored) it never emits — the next rule wins.
  std::vector<scilex::rule> rules;
  rules.push_back({.kind = 1, .pattern = real::regex(""), .skip = false});
  rules.push_back({.kind = 2, .pattern = real::regex("[0-9]+")});
  const scilex::lexer lexer(std::move(rules));

  const auto tokens = lexer.tokenize("42");
  EXPECT_EQ(tokens.size(), 1U);
  EXPECT_EQ(tokens[0].kind, 2);
}

TEST(nullable_rule_is_general_and_tried_everywhere)
{
  // A nullable pattern ([0-9]* also matches the empty string) has no usable
  // first-byte set, so it joins the general list and is tried at EVERY position: it
  // emits where it matches non-empty, and its empty match is ignored elsewhere. This
  // is the case the general list still serves under exact dispatch.
  std::vector<scilex::rule> rules;
  rules.push_back({.kind = 1, .pattern = real::regex("[0-9]*"), .skip = false}); // nullable -> general
  rules.push_back({.kind = 2, .pattern = real::regex("[a-z]")});                 // bucket a-z
  const scilex::lexer lexer(std::move(rules));

  const auto tokens = lexer.tokenize("5a");
  EXPECT_EQ(tokens.size(), 2U);
  EXPECT_EQ(tokens[0].kind, 1);          // "5"  -> nullable rule via the general list (non-empty match)
  EXPECT_EQ(tokens[0].lexeme, "5"sv);
  EXPECT_EQ(tokens[1].kind, 2);          // 'a': [0-9]* matches empty (ignored) -> the letter wins
  EXPECT_EQ(tokens[1].lexeme, "a"sv);
}

TEST(optional_leading_literal_buckets_all_its_first_bytes)
{
  // "a?b" can begin with 'a' or 'b' (the 'a' is optional) but is NOT nullable (the
  // 'b' is required), so REAL's exact first-byte set is {a, b} -> bucketed under
  // both. It matches "b" (where a naive single-byte bucket would have skipped it)
  // as well as "ab".
  std::vector<scilex::rule> rules;
  rules.push_back({.kind = 1, .pattern = real::regex("a?b"), .skip = false});
  rules.push_back({.kind = 2, .pattern = real::regex("[a-z]")});
  const scilex::lexer lexer(std::move(rules));

  EXPECT_EQ(lexer.tokenize("b").at(0).lexeme, "b"sv);   // matched at 'b' -> kind 1
  EXPECT_EQ(lexer.tokenize("b").at(0).kind, 1);
  EXPECT_EQ(lexer.tokenize("ab").at(0).lexeme, "ab"sv); // matched at 'a' -> kind 1
}

TEST(alternation_buckets_each_branch_first_byte)
{
  // "cat|dog" has first bytes {c, d}, so it is bucketed under both branches' leads
  // and matches either.
  std::vector<scilex::rule> rules;
  rules.push_back({.kind = 0, .pattern = real::regex("\\s+"), .skip = true});
  rules.push_back({.kind = 1, .pattern = real::regex("cat|dog"), .skip = false});
  rules.push_back({.kind = 2, .pattern = real::regex("[a-z]+")});
  const scilex::lexer lexer(std::move(rules));

  EXPECT_EQ(lexer.tokenize("cat").at(0).kind, 1);
  EXPECT_EQ(lexer.tokenize("dog").at(0).kind, 1); // matched at 'd' -> the second branch
}

TEST(tie_break_favours_the_earlier_rule)
{
  // A class rule and a bucketed literal both end up in bucket 'i'; on an equal-length
  // tie the earlier rule in rules_ (lower index) must win — priority is index order,
  // not the order the buckets happen to be scanned.
  std::vector<scilex::rule> rules;
  rules.push_back({.kind = 1, .pattern = real::regex("[a-z]+")}); // earlier in rules_
  rules.push_back(lit(2, "if"));                                  // bucket 'i', later in rules_
  const scilex::lexer lexer(std::move(rules));

  EXPECT_EQ(lexer.tokenize("if").at(0).kind, 1);                  // "[a-z]+" wins the len-2 tie (earlier rule)
}

TEST(tie_break_favours_an_earlier_general_rule_over_a_later_bucket)
{
  // A nullable rule (general list, low index) and a bucketed rule (higher index) can
  // tie on length at a position. The bucket is scanned first, yet the earlier rule in
  // rules_ must still win — the index tie-break crosses the bucket/general boundary.
  std::vector<scilex::rule> rules;
  rules.push_back({.kind = 1, .pattern = real::regex("[a-z]*")}); // nullable -> general (index 0)
  rules.push_back({.kind = 2, .pattern = real::regex("[a-z]")});  // bucket a-z (index 1)
  const scilex::lexer lexer(std::move(rules));

  const auto tokens = lexer.tokenize("a");
  EXPECT_EQ(tokens.size(), 1U);
  EXPECT_EQ(tokens.at(0).kind, 1);     // both match "a" (len 1); the earlier rule wins
  EXPECT_EQ(tokens.at(0).lexeme, "a"sv);
}

TEST(lexer_is_copyable)
{
  // Storing indices (not pointers) into rules_ makes the lexer a value type again:
  // it can live in a container, and the copy tokenizes identically to the original
  // (the deliverable of the rewrite; copy/move assignability is asserted in
  // test_lexer.cpp). A std::vector<lexer> only compiles if the lexer is copyable,
  // and push_back of an lvalue exercises the copy constructor.
  std::vector<scilex::rule> rules;
  rules.push_back({.kind = 0, .pattern = real::regex("\\s+"), .skip = true});
  rules.push_back(lit(1, "if"));
  rules.push_back({.kind = 2, .pattern = real::regex("[a-z]+")});
  const scilex::lexer original(std::move(rules));

  std::vector<scilex::lexer> copies;
  copies.push_back(original); // requires + exercises copy construction
  EXPECT_EQ(copies.at(0).tokenize("if x").size(), 2U);
  EXPECT_EQ(copies.at(0).tokenize("if x").at(0).kind, original.tokenize("if x").at(0).kind);
  EXPECT_EQ(copies.at(0).tokenize("if x").at(1).lexeme, "x"sv);
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
