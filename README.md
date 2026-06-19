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
- Optional indentation layout (`scilex::layout`, an opt-in header): rewrites a
  token stream with synthetic `newline` / `indent` / `dedent` tokens for
  indentation-significant languages; throws `layout_error` on an inconsistent
  dedent.
- Lexical errors as exceptions carrying the failing position (`lex_error`).
- Linear-time / ReDoS-safe tokenization, inherited from REAL.

**Not in v1 (and honestly excluded — no phantom features):**

- Modes / context-sensitive lexing.
- Compile-time `static_lexer` (built on REAL's `static_regex`).
- Codepoint columns (columns are byte-based, matching REAL's UTF-8 model).
- Command-line tool, JSON specification language.

These may come later, each only if it earns its place — measured, tested, and
kept minimal.

## Dependencies

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
pip.

For CI or a reproducible build — where no on-disk layout can be assumed — fetch
REAL with CMake FetchContent instead (`make build FETCH=1`, or
`-DSCILEX_FETCH_DEPS=ON`); point it at a remote and pin a tag with
`-DSCILEX_REAL_REPO=https://… -DSCILEX_REAL_TAG=v2026.6.6`.

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

`scilex::scilex` is the CMake target — `add_subdirectory`, `FetchContent`, or an
installed config package. The config calls `find_dependency(real)`, so installing
REAL's config package alongside (on the same prefix) makes the whole chain
resolve from one `find_package`:

```cmake
# With REAL and SciLex installed under <prefix>:
find_package(scilex CONFIG REQUIRED)   # pulls in real:: transitively
target_link_libraries(app PRIVATE scilex::scilex)
```

## Releasing

`make release` computes the next calendar version `YYYY.M.PATCH` — the patch
resets each month, the first release of a month is `.0` (PEP 440 drops leading
zeros, so `2026.6.1`, never `2026.06.001`) — bumps it in `pyproject.toml` and
`python/scilex/__init__.py`, then commits, tags and pushes from a clean `main`.
The pushed tag drives `.github/workflows/release.yml`,
which checks the tag matches the version, builds abi3 wheels (`cibuildwheel`,
Linux/macOS/Windows) and the self-contained sdist, builds the full **API reference**
(Doxygen + the embedded coverage report + the guided tour), and publishes the
distributions to PyPI via Trusted Publishing (OIDC, no stored secret). It then
populates the GitHub `/releases` page with auto-generated notes, the wheels/sdist,
and the API-reference tarball (`scilex-doc.tar.gz`). The pushed tag is the single
thing that triggers a publish; SciLex remains consumable as source too (sibling
checkout / FetchContent / `get_include()`).

A separate `.github/workflows/docs.yml` workflow also deploys the API reference to
**GitHub Pages** on each push to `main` (enable Pages once with "GitHub Actions" as
the source).

> **Note.** The first release, `v2026.6.0`, predates this docs step — it shipped
> wheels + sdist only. The API-reference artifact and the Pages deployment were added
> immediately after and apply to subsequent releases.

**One-time PyPI setup.** Publishing needs a PyPI
[Trusted Publisher](https://docs.pypi.org/trusted-publishers/) configured once for
the project (publisher `RECHE23/scilex`, workflow `release.yml`, environment
`pypi`) and a matching `pypi` GitHub environment — no API token is stored.

## Python binding

SciLex ships an abi3 CPython extension (use the C++ lexer from Python).
`pip install scilex` installs one `cp310-abi3` wheel per platform (CPython 3.10+;
the self-contained sdist compiles where no wheel matches, pulling REAL's headers
from the `real-regex` build dependency). For a source checkout:

```bash
make python        # build the extension in place
make python-test   # run the binding test suite
```

```python
import scilex
lx = scilex.Lexer([
    (0, r"\s+", True),                       # (kind, pattern, skip) — skipped
    (1, r"[0-9]+", False),                   # number
    (2, r"[A-Za-z_][A-Za-z0-9_]*", False),   # identifier
])

# Eager: a list of rich Token objects (kind, lexeme, structured position).
[(t.kind, t.lexeme) for t in lx.tokenize("foo 42")]      # [(2, 'foo'), (1, '42')]

# Lazy: a generator yielding one Token at a time — nothing else is held.
for tok in lx.scan("foo 42"):
    tok.kind, tok.lexeme, tok.position.line, tok.position.column

# A lexical error carries the failing position.
try:
    lx.tokenize("foo @")
except scilex.error as e:
    e.position                                            # Position(line=1, column=5, offset=4)

# eof=True appends a terminal END_OF_INPUT token (a parser always has a token).
lx.tokenize("42", eof=True)[-1].kind == scilex.END_OF_INPUT
```

For indentation-significant languages, `Layout` rewrites an `eof=True` token
stream with `NEWLINE` / `INDENT` / `DEDENT` tokens read from each line's leading
column:

```python
lx = scilex.Lexer([(0, r"\s+", True), (1, r"\w+", False), (2, r":", False)])
laid = scilex.Layout().apply(lx.tokenize("if x:\n    a\nb", eof=True))
[t.kind for t in laid]
# [1, 1, 2, NEWLINE, INDENT, 1, NEWLINE, DEDENT, 1, NEWLINE, END_OF_INPUT]
```

`scilex.get_include()` returns the header directory so a C++ project can compile
against SciLex located through its Python install (add `real.get_include()` too,
since SciLex's headers include REAL's).

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

## Design

A guided tour of how SciLex works — maximal munch, the REAL foundation,
indentation layout, the C++/Python API, and the honest known limitations — lives in
[`docs/design.dox`](https://github.com/RECHE23/scilex/blob/main/docs/design.dox),
rendered into the API reference by `make doc`.

## Performance

See [BENCHMARKS.md](BENCHMARKS.md) for a reproducible, honest baseline (`make
bench`). The short of it: on benign input Python's `re` is faster (a mature C
backtracking engine), but SciLex is **linear-time and ReDoS-safe by construction**
— on a pathological pattern like `(a+)+b` it stays flat (~78 µs at 1000 chars)
where `re` explodes exponentially (seconds, then never finishing). SciLex trades
raw throughput on easy inputs for a guarantee that holds on every input.

## License

MIT — see [LICENSE](LICENSE).

## Author

René Chenard — rene.chenard@gmail.com
