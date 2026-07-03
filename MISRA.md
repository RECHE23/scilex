# MISRA C++:2023 deviations

`make misra` runs clang-tidy with the **shared SciForge profile** (`lint/clang-tidy-misra` in
SciForge), which covers a material subset of MISRA C++:2023 through the cppcoreguidelines, cert,
bugprone, hicpp and misc modules. The scope is SciLex's own headers (`--header-filter='include/scilex/.*'`;
a synthetic translation unit instantiates the lexer so the code is checked). The profile's deviations —
the checks left disabled there because they conflict with idiomatic header-library choices — apply to
SciLex as they do to the rest of the ecosystem and are documented in SciForge's own MISRA notes.

SciLex adds no repo-specific disabled checks. It carries exactly **two** in-source suppressions, each a
proven false positive with a structural reason to suppress rather than rewrite:

## Suppressions

### `clang-analyzer-core.NonNullParamChecker` (lexer.hpp)

In the shared per-rule munch, the analyzer cannot see that the pattern pointer is non-null on the path
that reaches `rules_[idx].pattern.match(rest)`, and flags a possible null dereference. It is a proven
false positive: the index comes from the dispatch tables, which only ever hold valid rule indices. A
defensive bounds/null guard here would be an **unreachable branch**, which the 100%-4D coverage gate
rejects — so the false positive is suppressed at the site rather than papered over with dead code.

### `bugprone-random-generator-seed` / `cert-msc32-c` / `cert-msc51-cpp` (lexer.hpp)

The build-time DFA equivalence audit uses a `std::mt19937` seeded with a **fixed constant** to generate
local probe strings. The fixed seed is correct by design: the audit must be reproducible, and the RNG
has **no security role** — it produces no tokens, identifiers, or cryptographic material. A
non-deterministic seed would make the audit flaky for no benefit.

Every other MISRA-relevant concern is handled by keeping the check enabled and satisfying it, not by
deviating.
