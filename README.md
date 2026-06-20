# SciLex

**A small, header-only C++20 maximal-munch lexer built on REAL.**

- Linear-time and ReDoS-safe by construction (via REAL).
- Eager `tokenize` or lazy `scan`.
- Optional indentation layout for significant-whitespace languages.
- C++20 header-only + abi3 Python binding (CPython 3.10+).
- Zero dependencies beyond REAL headers.

Define an ordered set of token rules — each a `(kind, regex, skip)` triple — and
SciLex tokenizes by **maximal munch**: the longest anchored match wins, with rule
order breaking ties. Because it is a thin layer over REAL, tokenization is linear
and ReDoS-safe by construction.

This follows the same design principles as REAL: purity, simplicity, and
measured optimality.

## Capabilities

- Ordered token rules: `(kind, real::regex, skip)`
- Maximal-munch matching (longest match wins, rule order for ties)
- Source positions (byte offset, line, column)
- Eager (`tokenize`) and lazy (`scan`) APIs
- Optional `END_OF_INPUT` token
- Optional indentation layout (NEWLINE / INDENT / DEDENT)
- Positioned errors with context snippet
- Linear-time / ReDoS-safe (via REAL)

**Not yet:** modes, `static_lexer`, codepoint columns.

See the [guided tour](docs/design.dox) for details.

## C++ API

```cpp
#include <scilex/scilex.hpp>

std::vector<scilex::rule> rules = {
    {0, real::regex("\\s+"), true},           // whitespace (skip)
    {1, real::regex("if")},                   // keyword before identifier
    {2, real::regex("[a-z_][a-z0-9_]*")},     // identifier
    {3, real::regex("[0-9]+")},               // number
};

scilex::lexer lexer(std::move(rules));

// Eager
for (const auto& tok : lexer.tokenize("if x + 42")) { ... }

// Lazy (preferred for parsers)
for (const auto& tok : lexer.scan("if x + 42")) { ... }
```

See [`docs/design.dox`](docs/design.dox) for the complete C++ API (`lexer`, `token`, `position`, `layout`, `lex_error`).

## Python binding

An abi3 CPython extension (CPython 3.10+, Limited API).

```python
import scilex

lx = scilex.Lexer([
    (0, r"\s+", True),                 # whitespace (skip)
    (1, r"[0-9]+", False),             # number
    (2, r"[A-Za-z_][A-Za-z0-9_]*", False),
])

# Eager
tokens = lx.tokenize("foo 42", eof=True)

# Lazy (generator)
for tok in lx.scan("foo 42"):
    print(tok.kind, tok.lexeme, tok.position)

# Errors with context
try:
    lx.tokenize("foo @")
except scilex.error as e:
    e.position
    e.context
```

For significant indentation:

```python
laid = scilex.Layout().apply(lx.tokenize(src, eof=True))
```

`pip install scilex` (wheels + sdist). Use `scilex.get_include()` to compile C++ code against the installed headers.

Build locally: `make python && make python-test`.

## CLI

`scilex` is a command-line lexer — `make cli` builds it, `make install` puts it on
your `PATH` (`PREFIX=`/`BINDIR=` to choose where). It has two input modes.

**Built-in grammars** — a showcase over the seven example languages (JSON, Python,
C++, SQL, CSS, Lisp, math):

```console
$ scilex --list                       # the built-in grammars
$ scilex --example json file.json     # lex a file …
$ scilex --example python --layout    # … or its bundled sample, with INDENT/DEDENT
```

**Your own grammar** — the universal mode: bring a `.lex` file and lex anything.
A grammar is one rule per line — `name`, a tab, `regex`, then an optional tab and
`skip` (`#` comments and blank lines are ignored):

```console
$ cat my.lex
WS	\s+	skip
NUMBER	[0-9]+(\.[0-9]+)?
IDENT	[A-Za-z_][A-Za-z0-9_]*
OP	<=|>=|==|!=|[-+*/%=<>]

$ echo 'x = 41 + 1' | scilex my.lex        # stdin when no file is given
IDENT	x	1:1
OP	=	1:3
NUMBER	41	1:5
OP	+	1:8
NUMBER	1	1:10
```

Output is one token per line — the kind, a tab, the lexeme, a tab, then `line:col`;
`--layout` adds the indentation tokens. A malformed grammar is reported with a
clear, positioned error (`my.lex:3: invalid regex: …`) — never a crash. See
`examples/sample.lex` for a worked file.

This `.lex` format is a *tool* convenience parsed by the CLI; the library itself
stays plain C++ rule lists (`std::vector<scilex::rule>`) — no spec language is
embedded.

## Dependencies

SciLex is header-only and depends only on REAL's headers (the package
`real-regex` on PyPI / https://github.com/RECHE23/real-regex).

By default the build looks for them in a sibling checkout:

```
~/Projects/
├── real-regex/   # REAL (https://github.com/RECHE23/real-regex)
└── scilex/       # SciLex  (uses ../real-regex/include by default)
```

Point the build elsewhere with `REAL_INCLUDE` (Makefile) or
`-DSCILEX_REAL_INCLUDE=...` (CMake) — for instance at the path printed by
`python -c "import real; print(real.get_include())"` when REAL is installed via
pip.

For CI or a reproducible build — where no on-disk layout can be assumed — fetch
REAL with CMake FetchContent instead (`make build FETCH=1`, or
`-DSCILEX_FETCH_DEPS=ON`); point it at a remote and pin a tag with
`-DSCILEX_REAL_REPO=https://… -DSCILEX_REAL_TAG=v2026.6.8`.

## Development

```bash
make test        # build and run the test suite
make coverage    # line-coverage summary + HTML report
make sanitize    # tests under AddressSanitizer + UndefinedBehaviorSanitizer
make lint        # clang-tidy
make format      # uncrustify, in place
make doc         # API reference (Doxygen) with embedded coverage
```

The API reference is published at <https://reche23.github.io/scilex/>.

Override the compiler with `make test CXX=g++-14`.

**Coverage bar.** SciLex holds the SciLang-stack gate — **100% on all four
dimensions** (lines, functions, regions and branches) of `include/`, checked by
`make coverage` and enforced by `make full-local-gate` (using Apple clang 16).
The published report on GitHub Pages / the doc tarball (built on clang 18) reads
mid-90s (newer clang instruments more branches). This is the documented toolchain
distinction; see the live report for exact figures. (REAL is the other documented
exception to the 100% gate — see its README.)

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

`make release` computes the next calendar version `YYYY.M.PATCH` (the patch resets
each month; PEP 440 drops leading zeros). The pushed tag drives the release workflow
— wheels + sdist + the API-reference tarball + a GitHub Release, published via Trusted
Publishing — while `docs.yml` deploys the reference to GitHub Pages.

## Design

A guided tour of how SciLex works (maximal munch, REAL foundation, layout,
C++/Python API, current scope) lives in
[`docs/design.dox`](docs/design.dox) (also rendered by `make doc`).

## Performance

See [BENCHMARKS.md](BENCHMARKS.md). On normal input `re` is faster. On adversarial
input SciLex stays linear while `re` explodes. See the benchmarks for details.

## License

MIT — see [LICENSE](LICENSE).

## Author

René Chenard
