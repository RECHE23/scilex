# SciLex

**A small, header-only C++20 contextual lexer built on REAL.**

- ReDoS-safe by construction (via REAL): no rule backtracks, nothing is exponential. Linear on
  every rule a DFA takes and on grammars whose rules stop scanning near their tokens; quadratic in the
  worst case, through a rule left on Pike (see *Performance*).
- **Modes** — contextual lexing: the same byte lexes differently by context
  (f-strings, XML tag/content, YAML block/flow).
- **Layout Awareness** — mode-aware indentation (NEWLINE / INDENT / DEDENT).
- Eager `tokenize` or lazy `scan`; positioned errors with a context snippet.
- C++20 header-only + abi3 Python binding (CPython 3.11+).
- Zero dependencies beyond REAL headers.

Define an ordered set of token rules — each a `(kind, regex, skip)` triple — and
SciLex tokenizes by **maximal munch**: the longest anchored match wins, with rule
order breaking ties. A rule can also opt into **modes** (contextual lexing), so the
same byte lexes differently by context. Because it is a thin layer over REAL,
every rule match is linear in what it scans and ReDoS-safe by construction; tokenizing is linear
wherever the rules run on the DFA, and quadratic in the worst case only through a rule left on Pike
(see *Performance*).

What that covers today: significant indentation, plus contexts like f-strings, YAML
flow collections, and bracket continuation (modes + **Layout Awareness Level A**).
Cases that need a deeper lexing↔indentation coupling — YAML block scalars `|` / `>`,
heredocs — are **Level B**: documented, not in this version.

This follows the same design principles as REAL: purity, simplicity, and
measured optimality.

## Capabilities

- Ordered token rules: `(kind, real::regex, skip)`
- Maximal-munch matching (longest match wins, rule order for ties)
- **Contextual lexing (modes)** — per-rule `in_mode` + a push / pop / set mode stack
- **DFA fast path (automatic)** — every mode whose DFA reproduces the per-rule munch is accelerated 3–27× (dense grammars ~15–27×) with one `real::dfa` pass; the decision is exact (Pike is the floor), the token stream identical; `dfa_policy::requested` restricts it to `dfa_modes`
- **Layout Awareness** — mode-aware indentation (NEWLINE / INDENT / DEDENT)
- Source positions (byte offset, line, column); each token carries its mode
- Eager (`tokenize`) and lazy (`scan`) APIs
- Optional `END_OF_INPUT` token
- Positioned errors with a context snippet
- ReDoS-safe (via REAL); linear wherever the rules run on the DFA, quadratic in the worst case only through a rule left on Pike
- Nine example grammars — three of them modal (f-strings, XML, YAML)

The three modal grammars differ in shape and each documents its own scope; modes
resolve the contexts above, but the one contextual case still outside the model —
lexing steered by *indentation* (block scalars, heredocs) — is Level B.

**Not yet:** block scalars / heredocs (Layout Awareness Level B), a compile-time
`static_lexer` (a baked DFA — the Phase-0 spike found this wants build-time codegen,
not constexpr), codepoint columns.

See the [guided tour](docs/design.dox) for details.

## C++ API

```cpp
#include <scilex/scilex.hpp>

std::vector<scilex::rule> rules {
  {.kind = 0, .pattern = real::regex(R"(\s+)"), .skip = true}, // whitespace, skipped
  {.kind = 1, .pattern = real::regex("if")},                   // keyword: listed before the identifier
  {.kind = 2, .pattern = real::regex("[a-z_][a-z0-9_]*")},     // identifier
  {.kind = 3, .pattern = real::regex("[0-9]+")},               // number
  {.kind = 4, .pattern = real::regex(R"([-+*/=])")},           // operator
};
const scilex::lexer lexer {std::move(rules)};

// Lazy: one token per step (tokenize() returns them all at once).
for (const scilex::token& tok : lexer.scan("if x + 42")) {
  std::printf("%d %.*s\n", tok.kind, static_cast<int>(tok.lexeme.size()), tok.lexeme.data());
}
```

See [`docs/design.dox`](docs/design.dox) for the complete C++ API (`lexer`, `token`, `position`, `layout`, `lex_error`).

## Python binding

An abi3 CPython extension (CPython 3.11+, Limited API).

```python
import scilex

lx = scilex.Lexer([
    (0, r"\s+", True),                 # whitespace (skip)
    (1, r"[0-9]+", False),             # number
    (2, r"[A-Za-z_][A-Za-z0-9_]*", False),
])
# Every mode whose DFA is exact is accelerated (3–27×): lx.dfa_modes_active names them;
# scilex.Lexer([...], dfa="requested") keeps the per-rule path.

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

## Contextual lexing — modes

A flat rule list can't separate contexts where the *same* byte means different
things — `{` opens a Python f-string interpolation but a dict elsewhere; `<` opens
an XML tag in content but is just a character inside CDATA. SciLex handles this with
an opt-in **mode stack**: a rule may be restricted to named modes (`in_mode`) and
may push / pop / set the mode when it wins. The engine is unchanged — maximal munch
and the exact first-byte dispatch simply run *per mode*.

This unlocks, with no engine change:

- **f-strings** — `f"sum={a+b}"`: code ↔ string body ↔ interpolation, nesting
  through the stack;
- **XML** — `content ↔ tag` (a shallow two-mode flip; CDATA and comments are single
  regex tokens, so an inner `<` is literal);
- **YAML** — `block ↔ flow` (significant indentation plus flow collections).

```cpp
using op = scilex::mode_action::op;
scilex::rule open {.kind = OPEN, .pattern = real::regex("f\"")};
open.in_mode = {"default", "interp"};                      // active in code
open.action  = {.operation = op::push, .target = "fstr"};  // enters the f-string body
// "{" pushes "interp"; the closing quote pops "fstr"; the stack tracks nesting.
```

```python
NAME, OPEN, TEXT, LB, RB, CLOSE = range(6)
fstr = scilex.Lexer([
    (NAME, r"[a-z]+", False, ["default", "interp"]),               # code, shared
    (OPEN, r'f"', False, ["default", "interp"], ("push", "fstr")),
    (TEXT, r'[^{}"]+', False, ["fstr"]),
    (LB, r"\{", False, ["fstr"], ("push", "interp")),         # "{" opens it from the body
    (CLOSE, r'"', False, ["fstr"], ("pop",)),
    (RB, r"\}", False, ["interp"], ("pop",)),
])
[t.kind for t in fstr.tokenize(r'f"hi {name}"')]   # OPEN TEXT LB NAME RB CLOSE
```

An action is `None` | `("push", mode)` | `("set", mode)` | `("pop",)`; a plain
`(kind, pattern, skip)` rule needs neither field, so existing grammars are
unaffected. See `examples/python.hpp`, `examples/xml.hpp`, `examples/yaml.hpp` for
the three modal profiles in full. The stack is bounded: a push past
`scilex::max_mode_depth` (65 536 frames, ~2 MiB) is a lexical error under either error
policy, so an input made only of openers cannot grow it without end.

## DFA fast path (automatic)

Every mode is accelerated by a `real::dfa` where that is exact: instead of trying each candidate rule at
every position, one DFA pass recognizes the winning rule — the same maximal munch,
with the order tie-break baked into the automaton. On a mode where many rules share
leading bytes that is **3–27× the regular path** on the full token path (dense grammars ~15–27×).

```cpp
scilex::lexer lexer(std::move(rules));   // dfa_policy::automatic: every mode is tried
lexer.dfa_modes_active();                // the modes actually accelerated
// Only some modes, or none: dfa_policy::requested with the names (empty = the per-rule path).
scilex::lexer pike(std::move(other_rules), {}, {}, scilex::error_policy::raise,
                   scilex::column_unit::bytes, scilex::dfa_policy::requested);
```

It is **best-effort and invisible**: a rule that needs a zero-width assertion no DFA can
represent, or whose DFA would change an answer, silently stays on the regular Pike engine
beside its mode's DFA (`pike_rules(mode)` names it). A DFA takes each rule's *longest* match
while Pike takes the match the rule's priority order prefers, and which rules keep the
two equal is not visible in the syntax: `as|assert` stops at `as` on "assert" and so
stays on Pike, while the lazy `x*?y` agrees on every input and keeps it. The
constructor **decides** this for every rule with `real::dfa_faithful` — exactly, not by
sampling — so the **token stream is byte identical** either way (Pike is the floor) and
`layout` is unchanged. The DFA is built once, in the
constructor, and that is its cost: measured 2026-09-24 (arm64, `-O2`, minimum of 7, REAL
`2026.9.7`), building the example grammars' lexers takes 0.07–5.6 ms instead of 0.01–0.12 ms, and the
Python grammar's five modes ~26 ms (~140 ms against REAL `2026.9.6`, whose DFA construction was
slower). A caller that builds many short-lived lexers can pass
`dfa_policy::requested`. From Python: `Lexer(..., dfa="requested")`.

## Unicode identifiers vs DFA speed — the grammar author's choice

A real trade-off worth stating plainly. Write an identifier rule as `\w+` (or `[^\W\d]\w*`)
with the default flags and it reads **Unicode identifiers** — `café`, `変数` — the faithful
behaviour for a language like Python 3. But a Unicode `\w` expands into more UTF-8 byte
transitions than a DFA is built from, and `\b` is a zero-width assertion no DFA represents, so a
rule holding either **stays on the general engine** beside its mode's DFA (same tokens, visible via
`pike_rules(mode)`). The narrower Unicode `\d` and `\s` expand and stay on the DFA. Concretely the
general engine runs at **~7–14.5 MB/s** while a fully DFA-able mode runs **3–27× that**; the
`python-unicode` grammar, whose identifier rule stays on Pike, measured 36 MB/s against 10 MB/s for
Pike alone (2026-09-23, arm64, `-O2`, 1 MiB) — the Unicode identifier costs part of the DFA.

So: if your identifiers are ASCII by specification (JSON, SQL, C), pin **`(?a)`** inline in the
pattern (or pass `real::flags::ascii`) to keep `\w \d \s \b` ASCII, small, and DFA-representable —
what the `examples/` grammars do. If you want Unicode identifiers, write `\w+` and accept that its
rule runs on the general engine. The two tokenize ASCII input identically; they differ only on non-ASCII input
and on whether the mode can be a DFA. The **`python-unicode`** example (`scilex --example
python-unicode`) is the faithful-Python-3 variant of `python`, identical but for that one rule.

## Layout Awareness (Level A)

The layout pass is positional, and by default mode-blind. **Layout Awareness Level
A** lets a mode be marked *insignificant* (`Lexer(insignificant_modes=…)`), so its
tokens pass through without shaping indentation — and every token carries its `mode`
(`Token.mode`) for the pass to read.

That lifts two real cases a decoupled positional pass otherwise gets wrong:

- **YAML multi-line flow** — `[\n  1,\n  2\n]` adds no spurious INDENT/DEDENT;
- **Python implicit continuation** — a call/list/dict wrapped across lines inside
  `()` `[]` `{}` reads as continuation, not a new block.

```python
laid = lexer.layout(lexer.tokenize(src, eof=True))   # uses the lexer's own policy
```

Two invariants hold: with **no** insignificant mode the result is byte-for-byte the
positional pass (zero cost); and the **mode** is the single source of the policy (no
per-rule flag).

**Honest scope.** Level A covers multi-line flow and implicit continuation. **Block
scalars** (`|` / `>`) and heredocs need a reference indent carried in the mode frame
— that is **Level B**, a designed next step, not yet built. The bundled grammars
*demonstrate* the features; each `examples/<lang>.hpp` header documents its own scope.

## CLI

`scilex` is a command-line lexer — `make cli` builds it, `make install` puts it on
your `PATH` (`PREFIX=`/`BINDIR=` to choose where). It has two input modes.

**Built-in grammars** — a showcase over the nine example languages (JSON, Python,
C++, SQL, CSS, Lisp, math, XML, YAML):

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
`-DSCILEX_REAL_REPO=https://… -DSCILEX_REAL_TAG=v2026.9.6`.

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

See [BENCHMARKS.md](BENCHMARKS.md). On the benign case measured there SciLex is now 1.39× faster than
`re` (it was ~2× slower a stamp ago); on a ReDoS pattern SciLex stays linear while `re` explodes. See
the benchmarks for details.

**Linear on the DFA; the worst case is quadratic, and only on Pike.** Every rule match is linear in
the text it scans, but at each token start every candidate rule is tried, and a rule may scan far past
the token that finally wins: `a*b` and `a` on `aaa…` scan to the end looking for `b` at every
position, then lose to `a`. On the DFA each walk is memoized over the whole source (Reps, 1998): a
state a walk proved leads to no accept stops every later walk that reaches it, so the rules on the DFA
cost O(n × states) in total. A rule the DFA cannot take (see *DFA fast path*) keeps the per-position
scan, and the worst case with it. Measured on 2026-09-24 (arm64, Apple clang 16, `-O2`): `a*b` and `a` on `aaa…`, both on the DFA, lex 256 KiB in 8.8 ms and double with the input; the same pair kept on Pike (`dfa_policy::requested`) still quadruples per doubling, 266 ms at 4 000 bytes and 16.9 s at 32 000 (2026-09-23). The shipped grammars stay linear (flat MB/s in
[BENCHMARKS.md](BENCHMARKS.md)); a grammar fed by users (`.lex` files) reaches the worst case only
through a rule left on Pike (`pike_rules(mode)` names them). See [`docs/spec.dox`](docs/spec.dox).

## License

MIT — see [LICENSE](LICENSE).

## Author

René Chenard
