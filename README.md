# SciLex

A small, header-only C++20 lexer built on [REAL](https://github.com/RECHE23/real-regex).

Define an ordered set of token rules — each a kind paired with a REAL regular
expression — and SciLex tokenizes source text by **maximal munch** (longest
match wins, rule order breaks ties). Because REAL is a linear-time engine,
tokenization is linear and ReDoS-safe by construction: no token rule can make
the scanner backtrack catastrophically.

This is a deliberate fresh start under the same premises as REAL — purity,
simplicity, and measured optimality. SciLex is a thin layer over REAL, not a
re-implementation of pattern matching.

## Scope (v1)

**Included:**

- Token rules: a kind, a `real::regex`, and a `skip` flag (whitespace, comments).
- Maximal-munch tokenization with rule-order priority on equal-length ties.
- Source positions: byte `offset`, 1-based `line` and byte `column`.
- Non-owning tokens: each `lexeme` views into the source.
- Two ways to consume tokens: `tokenize` (eager, into a vector) and `scan`
  (a lazy single-pass range that produces one token at a time — no token
  vector is allocated; the parser-friendly access pattern).
- Optional synthetic end-of-input token (`eof_policy::append`): emits a final
  `end_of_input` token at the real end position, so a parser always has a
  current token to match.
- Lexical errors as exceptions carrying the failing position (`lex_error`).
- Linear-time / ReDoS-safe tokenization, inherited from REAL.

**Not in v1 (and honestly excluded — no phantom features):**

- Modes / context-sensitive lexing.
- Indentation tracking (INDENT/DEDENT tokens).
- Compile-time `static_lexer` (built on REAL's `static_regex`).
- Codepoint columns (columns are byte-based, matching REAL's UTF-8 model).
- Python binding, command-line tool, JSON specification language.

These may come later, each only if it earns its place — measured, tested, and
kept minimal.

## Dependency on REAL

SciLex is header-only and depends only on REAL's headers. By default the build
looks for them in a sibling checkout:

```
~/Projects/
├── real-v1/      # REAL
└── scilex-v1/    # SciLex  (uses ../real-v1/include by default)
```

Point the build elsewhere with `REAL_INCLUDE` (Makefile) or
`-DSCILEX_REAL_INCLUDE=...` (CMake) — for instance at the path printed by
`python -c "import real; print(real.get_include())"` when REAL is installed via
pip. Pinned, reproducible builds (CMake `FetchContent` against a REAL release
tag) will be wired in when SciLex cuts its first release.

## Build

```bash
make test        # build and run the test suite
make coverage    # line-coverage summary + HTML report
make sanitize    # tests under AddressSanitizer + UndefinedBehaviorSanitizer
make lint        # clang-tidy
make format      # uncrustify, in place
make doc         # API reference (Doxygen) with embedded coverage
```

Override the compiler with `make test CXX=g++-14`.

## Example

```cpp
#include <scilex/scilex.hpp>
#include <vector>

enum kind { WS, KW_IF, ID, NUM, PLUS };

std::vector<scilex::rule> rules;
rules.push_back({WS,    real::regex("\\s+"),  true}); // skipped
rules.push_back({KW_IF, real::regex("if")});          // before ID: wins ties
rules.push_back({ID,    real::regex("[a-z]+")});
rules.push_back({NUM,   real::regex("[0-9]+")});
rules.push_back({PLUS,  real::regex("\\+")});

const scilex::lexer lexer(std::move(rules));

// Eager: all tokens in a vector.
for (const scilex::token& t : lexer.tokenize("if x + 42")) {
    // t.kind, t.lexeme, t.start.{offset,line,column}
}

// Lazy: one token at a time, nothing else materialized.
for (const scilex::token& t : lexer.scan("if x + 42")) {
    // ...
}
```

## License

MIT — see [LICENSE](LICENSE).

## Author

René Chenard — rene.chenard@gmail.com
