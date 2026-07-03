// Exhaustive lexer oracle check: scilex::lexer vs reference.hpp (the executable spec), over an
// enumerated space of small rule-sets — the generalisation of oracle_check's fixed adversarial grammars.
//
// The space: every rule-set of 1..3 rules drawn (order-significant, repetition allowed, so tie-breaks
// are exercised) from a fixed micro-palette of NON-NULLABLE patterns (a nullable rule body is the
// fatal zero-length case #4, excluded here), each rule with or without `skip`, crossed with every input
// up to length n over Σ={a,b} plus one out-of-alphabet byte and one multi-byte code point. Both error
// policies are checked: raise (scilex::fuzz::check) and token/recovery (check_recover). The inputs come
// from the shared Python substrate (sciforge.corpus.exhaustive) — no re-enumeration here.
//
// The oracle is reference.hpp. Any divergence is a bug in ONE of the two (the lexer OR the reference);
// this harness only reports it — the triage decides which.
//
// Usage: exhaustive_lex <inputs-file>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "scilex/scilex.hpp"
#include "reference.hpp"

namespace {

  // Non-nullable tier-1 patterns over Σ={a,b}: each matches at least one byte, so no rule is a zero-
  // length (#4) body. They overlap deliberately (a vs [ab] vs ., a+ vs [ab]+) to stress maximal munch
  // and the earliest-rule tie-break.
  const char* const PALETTE[] {"a", "b", "ab", "[ab]", "a+", "[ab]+", "."};
  constexpr int     PALETTE_N {7};
  constexpr int     MAX_RULES {3};

  std::vector<scilex::rule> build_rules(const std::vector<int>& pattern_ids,
                                        unsigned                skip_mask)
  {
    std::vector<scilex::rule> rules;
    for (std::size_t i {0}; i < pattern_ids.size(); ++i) {
      rules.push_back({.kind    = static_cast<int>(i),
                       .pattern = real::regex(PALETTE[pattern_ids[i]]),
                       .skip    = (skip_mask & (1U << i)) != 0U});
    }
    return rules;
  }

  std::vector<std::string> read_inputs(const char* path)
  {
    std::vector<std::string> inputs;
    std::ifstream            in(path);
    std::string              line;
    while (std::getline(in, line)) {
      inputs.push_back(line);
    }
    // One multi-byte code point (é = C3 A9) alone and beside Σ bytes — the code-point advance path.
    inputs.emplace_back("\xC3\xA9");
    inputs.emplace_back("a\xC3\xA9");
    inputs.emplace_back("\xC3\xA9""b");
    return inputs;
  }

  // Enumerate every ordered pattern-id tuple of length `count` (base-PALETTE_N counting), invoking `fn`.
  template <typename Fn>
  void for_each_tuple(int count,
                      Fn  fn)
  {
    std::vector<int> ids(static_cast<std::size_t>(count), 0);
    while (true) {
      fn(ids);
      int pos {count - 1};
      while (pos >= 0 && ++ids[static_cast<std::size_t>(pos)] == PALETTE_N) {
        ids[static_cast<std::size_t>(pos)] = 0;
        --pos;
      }
      if (pos < 0) {
        break;
      }
    }
  }
} // namespace

int main(int    argc,
         char** argv)
{
  if (argc != 2) {
    static_cast<void>(std::fprintf(stderr, "usage: %s <inputs>\n", argv[0]));
    return 2;
  }
  const std::vector<std::string> inputs      {read_inputs(argv[1])};
  long long                      rule_sets   {0};
  long long                      cases       {0};
  long long                      divergences {0};
  int                            shown       {0};

  for (int count {1}; count <= MAX_RULES; ++count) {
    for_each_tuple(count, [&](const std::vector<int>& ids) {
                     for (unsigned skip_mask {0}; skip_mask < (1U << count); ++skip_mask) {
                       const std::vector<scilex::rule> rules {build_rules(ids, skip_mask)};
                       const scilex::lexer             raise_lex {std::vector<scilex::rule>(rules)};
                       const scilex::lexer             token_lex {std::vector<scilex::rule>(rules), {}, {},
                                                                  scilex::error_policy::token};
                       ++rule_sets;
                       for (const std::string& input : inputs) {
                         cases += 2;
                         const scilex::fuzz::result raise_out {scilex::fuzz::check(rules, raise_lex, input, false)};
                         const scilex::fuzz::result token_out {scilex::fuzz::check_recover(rules, token_lex, input)};
                         for (const auto& [outcome, policy] :
                              {std::pair {raise_out, "raise"}, std::pair {token_out, "token"}}) {
                           if (!outcome.ok) {
                             ++divergences;
                             if (shown < 25) {
                               std::string spec;
                               for (std::size_t i {0}; i < ids.size(); ++i) {
                                 spec += PALETTE[ids[i]];
                                 spec += (skip_mask & (1U << i)) ? "(skip) " : " ";
                               }
                               static_cast<void>(std::fprintf(stderr, "DIVERGE [%s] rules={%s} input-bytes=%zu inv=%s\n",
                                                              policy, spec.c_str(), input.size(), outcome.invariant));
                               ++shown;
                             }
                           }
                         }
                       }
                     }
                   });
  }

  static_cast<void>(std::printf("exhaustive-lex: %lld rule-sets, %lld checks, divergences=%lld\n",
                                rule_sets, cases, divergences));
  return divergences == 0 ? 0 : 1;
}
