# Changelog

All notable changes to SciLex. Versions are calendar-based (`YYYY.M.PATCH`, the
patch resetting each month; PEP 440 drops leading zeros). The project holds the
SciLang-stack gate: 100%-4D coverage of `include/`, dual-compiler, sanitizers, and a
fuzz oracle.

## Unreleased

### Changed
- **A token on the DFA costs what it did before the per-rule hybrid, within 4 % on x86-64.** A token
  whose rule carries no mode action no longer calls the out-of-line mode transition, a mode wholly on
  its DFA no longer carries the Pike merge's frame, and a scan makes one walk memo per mode up front.
  callgrind on 1 MiB of JSON, x86-64, against the same REAL headers: 125.1 M instructions at 2026.9.1
  → 111.9 M (g++ 13.3 -O2; 107.8 M before the hybrid), 133.7 M → 110.3 M (clang 18; 106.8 M before).
  arm64 keeps more of the gap: 2.22 G instructions retired on 20 MiB (Apple clang), 1.99 G before the
  hybrid.
- **The libFuzzer target runs ~7× more inputs per second** (0.7 → ~5 under ASan + UBSan, x86-64). It
  built four lexers, DFAs included, for every input; the seeded rule-sets now come from 256 slots per
  mode built once, each input checks three of the nine example grammars in a rotation its hash picks
  (`make fuzz-check` still runs all nine), and `-max_len` is 2 KiB (`FUZZ_MAX_LEN`). What remains is
  the independent reference's per-call cost in REAL.
- CI runs the fuzz oracle (fuzz-check and a two-minute libFuzzer smoke), coverage under Linux clang
  18 against the local gate's 100 % bar (`make coverage-gate`), and the test suite built and run as
  32-bit (i686); a weekly workflow fuzzes for an hour from a corpus kept between runs.

### Added
- **The `.lex` grammar format is a library header**, `scilex/grammar.hpp` (optional; `scilex.hpp` does
  not include it): `parse_grammar(text, origin)` and `load_grammar(path)` return the rules and their
  names, and a malformed grammar raises `scilex::grammar_error` with its line and column. The format
  gains modes — `in=m1,m2`, `push=m`, `set=m`, `pop` beside `skip` — and every existing `.lex` file
  parses as before. The CLI uses it in place of its own parser; Python reaches it through
  `scilex.parse_grammar`, `scilex.load_grammar`, `Grammar` and `scilex.GrammarError`.
- `lexer::end_of(source, token)` (Python: `Lexer.end_of(source, token)`): the position just past a
  token, in the lexer's column unit -- exactly where the scan stood after it. Tokens keep carrying
  only their start, so the stream costs nothing more for callers that never ask; a token that was not
  lexed from `source` is refused (`std::invalid_argument`, Python `ValueError`).
- `scilex/version.hpp` (`SCILEX_VERSION_MAJOR/MINOR/PATCH`, `SCILEX_VERSION_STRING`), included by
  `scilex.hpp`, rewritten by `make release` and checked by `make version-check`; `scilex --version`
  prints it with the REAL version the CLI was built with.
- Python: `scilex.LexError` (input no rule can lex) and `scilex.LayoutError` (indentation layout
  refuses), both subclasses of `scilex.error`, so a caller can tell a lexing failure from a layout
  one; an invalid pattern stays a plain `scilex.error`.
- The thread-safety contract, stated in the README and on `lexer`: one const lexer, any number of
  threads, one iterator per thread.
- `make tsan`, run by the local gate and a CI job: eight threads share each fresh lexer and start at
  once on a barrier, under ThreadSanitizer, over a DFA, a modal, a layout and a hybrid grammar, and
  each thread's tokens must equal the single-threaded ones. The contract above was stated from a
  one-off check; this is what now holds it.

### Fixed
- **Python: a positioned error keeps its cause.** `Lexer.tokenize` and `Lexer.scan` rewrote every
  positioned error as "no rule matches at line …", so a pop at the root, a zero-length match, an
  unterminated mode or a mode stack too deep all read as a missing rule. The message is now the
  lexer's own, followed by the position and the context snippet.
- **The xml example grammar is linear under error recovery.** Its comment and CDATA rules were lazy,
  so they ran on Pike and rescanned the rest of the input at every `<`: 16 KiB of `<!-- x>` took
  3.5 s under `error_policy::token`. They are rewritten so that `match()` is the longest match —
  the same matches on every suffix checked, and now on the DFA. `fuzz-check` gains scaling cases.
- **The mode stack is bounded** by `scilex::max_mode_depth` (65 536 frames); a push past it is a
  `lex_error` under either policy. 16 MiB of `(` under the python grammar grew it past 1 GB.
- The README's C++ quickstart compiles (it is `examples/cpp/quickstart.cpp`, run by `make example`),
  the published API reference no longer carries private members, header sources or build-machine
  paths, and a GitHub release's notes are the tag's CHANGELOG section instead of a bare
  "Full Changelog" link.

## 2026.9.1 — 2026-09-24

### Changed
- **DFA acceleration is automatic.** The lexer tries every mode and keeps a `real::dfa` wherever the
  constructor decides it reproduces the per-rule munch exactly, so the token stream is unchanged.
  `scilex::dfa_policy::requested` (Python: `dfa="requested"`) restores the old behaviour: only the
  modes in `dfa_modes`, none if it is empty. The cost is DFA construction in every lexer's
  constructor: 0.07–5.6 ms for the example grammars, ~26 ms for Python's five modes, against REAL
  2026.9.7 (measured in the README).

- **A rule the DFA cannot take no longer sends its whole mode to Pike.** Each rule whose `match()` is
  its longest match joins the mode's DFA; the others (a `\b`, a `$`, a lookaround, a Unicode `\w`, a
  lazy delimiter) run on Pike beside it, and the munch merges the two by the per-rule rule: longest
  wins, the lowest index breaks a tie, a DFA rule's empty match competes too. The requirement that a
  mode with a nullable rule cover every byte is gone with it. `lexer::pike_rules(mode)` (Python:
  `Lexer.pike_rules(mode)`) names the rules left on Pike; `dfa_modes_active()` now lists every mode
  with at least one rule on a DFA. The `python-unicode` grammar lexes 3.5× faster.

- **Tokenizing is linear wherever the rules run on the DFA.** Each DFA walk is memoized over the
  whole source (REAL 2026.9.7's `real::dfa_munch_memo`), so `a*b` beside `a` over `aaa…` — n(n+1)/2
  steps before — lexes 256 KiB in 8.8 ms. The quadratic worst case remains only through a rule left
  on Pike. Requires `real-regex>=2026.9.7`.

### Added
- `scilex::layout(tokens, source, tab_policy, mode_significant)`: `tab_policy::python` measures
  indentation as CPython does (tab stops of 8, cross-checked with tabs as 1) and refuses an
  ambiguous mix of tabs and spaces with CPython's TabError message; `tab_policy::columns` is the
  existing pass. Python: `source=` and `tabs=` on `Lexer.layout`, `Layout.apply` and `layout()`.

### Fixed
- CLI: an invalid regex in a grammar is reported at its column in the line, with the bare cause;
  a lex error prints its cause as well as its position.

## 2026.9.0 — 2026-09-23

### Fixed

- **The DFA fast path could change the token stream**, in every release since it shipped (2026.6.6 to
  2026.8.0). A DFA takes each rule's *longest* match; the Pike munch takes the match each rule's priority
  order prefers. The constructor guarded the difference with a sampled audit (one- to eight-byte
  repeats of each possible first byte, plus 256 fixed-seed strings of at most 48 bytes), and the sample
  missed ordinary grammars: with keywords `as|assert`, an identifier rule and a catch-all, the DFA
  lexed `assert` as the keyword where Pike lexes the identifier, and the `xml`
  example's lazy CDATA rule ran past its first `]]>`. A catch-all rule puts every byte in the probe
  alphabet, which is where the sample stopped finding witnesses. `make fuzz` was red on its own seed
  corpus for this reason.
- The decision is now **exact**: every rule of an opted mode must satisfy `real::dfa_faithful` (is its
  `match()` always its longest match?), and a mode holding a rule that matches the empty string must
  make every byte a whole token of some rule, because the Pike munch lets a zero-length match win where
  the DFA reports none. Which rules pass is not syntactic -- the greedy `(?:ab|a)(?:bc)?` fails, the lazy
  `x*?y` passes. The shipped `sql` and `css` grammars stay accelerated.
- The DFA is built through `real::dfa`'s public constructor over the rules' regexes, so SciLex no longer
  reaches `real::detail::program_view`.
- **`layout` opened a line after a token spanning lines.** `x = """a\nb""" + y` emitted NEWLINE and
  INDENT before `+`, because the pass remembered where the last significant token *started*. It now
  remembers the line of its last byte; a token ending with `\n` still ends its line.

### Changed

- **Build requires `real-regex >= 2026.9.6`** (was `>= 2026.8.13`), the first release carrying
  `real::dfa_faithful`; the FetchContent tag moves with it.
- **The Python floor moves to CPython 3.11** (the wheel is `cp311-abi3`, the Limited API 3.11). It is
  inherited, not chosen: real-regex requires Python 3.11 from 2026.8.20 on, it is SciLex's build
  dependency, and cibuildwheel builds the abi3 wheel with the floor's interpreter -- a 3.10 build
  cannot install it. The 3.11 wheel was installed and exercised under CPython 3.11.5. `BENCHMARKS.md` is not re-measured in this
  release and keeps its 2026.8.13 stamp.
- **The complexity claim is corrected: SciLex is ReDoS-safe, not linear on every input.** The README,
  spec, design, comparison page, `BENCHMARKS.md`, package description and `SECURITY.md` promised
  "linear in the input for *every* input". Every rule match is linear in what it scans, but at each
  token start a rule may scan far past the token that wins, so tokenizing is O(n·S·m) and quadratic in
  the worst case: `a*b` and `a` on `aaa…` multiply the time by ~4 per doubling of the input on both the
  Pike and DFA routes (measured 2026-09-23, arm64, Apple clang 16, `-O2`). Nothing is exponential. The
  security policy now states that bound and scopes reports to super-quadratic scaling, a quadratic case
  in a shipped grammar, or any crash, hang or memory-safety issue.
- **Constructing a lexer with `dfa_modes` costs ~13–16 % more** on `css` and `sql` (median of 12
  interleaved pairs on arm64 at `-O2`, 2026-09-23: 9.55 → 11.09 ms and 10.97 → 12.37 ms; `json`
  unchanged). The sampled audit was cheap; the decision is ~1 ms per grammar. Paid once per lexer, not
  per token.
- The README and `rule` documentation said a Unicode `\w \d \s \b` keeps a mode off the DFA. Only `\w`
  (an expansion too wide to build) and `\b` (an assertion) do; Unicode `\d` and `\s` build.

## 2026.8.0 — 2026-08-11

### Changed

- **Build requires `real-regex >= 2026.8.13`** (was `>= 2026.7.37`), and the FetchContent tag moves with
  it. The sibling-checkout path already compiled against whatever REAL sat next door, so local builds had
  been getting these gains for a month; no reproducible build, no published wheel and no CI run had.

- **`BENCHMARKS.md` re-measured end to end** against real-regex 2026.8.13 on 2026-08-11, and its stamp now
  records **AC power** as a condition. That is not decoration: this host throttles ~29 % on battery, a
  first attempt at the refresh was measured there, and it would have published a 22 % *regression* on
  `json` that does not exist. An A/B is immune (the same comparison read +28.6 %–+100.2 % on battery
  against +28.9 %–+96.8 % on AC); an absolute table is not.

### Performance

- **The engine throughput table moves 29 % to 97 %**, measured five interleaved passes per side against
  real-regex 2026.7.37 and 2026.8.13 (run-to-run amplitude 1.01×–1.05×): `lisp` 7.29 → 14.35 MB/s,
  `math` 5.80 → 11.34, `css` 6.60 → 9.65, `sql` 6.91 → 9.65, `cpp` 7.27 → 10.11, `json` 7.00 → 9.02.
  A lexer validates short tokens one at a time — `.match()` per token — which is the per-CALL regime
  real-regex 2026.8.x spent itself on: a 4944-byte memset per state construction that only gcc emitted,
  an anchored fixed-shape route that used to decline outright, one of two bulk slot copies per call, and
  three routes that stopped paying a full engine entry per match. None of that shows in a throughput
  benchmark over 100 KB corpora, which is why REAL itself could not see most of it until it grew a
  per-call instrument. No part of the gain is attributed to any single release: the span is twenty of them.

- **Benign tokenization has crossed: SciLex is now 1.39× faster than `re`**, where the previous stamp had
  `re` ~2× ahead (0.996 ms against 1.380 ms, one run, side by side). Read narrowly — one case, 4000
  tokens over ~10 KB — and the durable part is the ratio inside the run, not the milliseconds. The
  headline that said SciLex "is not built to beat `re` on raw throughput, and it does not" has been
  updated to say what the measurement says.

- **The DFA speed-up range narrows to 2.8×–23.6×** (was 3.1×–26.7×) **because the Pike baseline caught
  up, not because the DFA slowed**: the DFA column is flat (`xml` 251.48 → 249.89, `math` 87.34 → 87.66)
  while the Pike column it is divided by rose (`lisp` 7.73 → 14.17, `math` 5.75 → 11.24). Full-set geomean
  ~9.7×, dense-set ~17.5×.

- The non-fail-fast O(remaining) figure reads **~451 000 ns/position** against the previous stamp's
  ~199 000, and that is **not** the engine: measured against real-regex 2026.7.37 and 2026.8.13 in one
  session it reads 446 284 and 452 417, a 1.4 % spread. Stable here across four passes, not
  reconstructible, so reported rather than explained; the quadratic shape this finding is about is
  unchanged.

## 2026.7.4 — 2026-07-12

### Fixed
- **`flags::ascii` grammars tokenized four control separators (FS/GS/RS/US, `U+001C`-`U+001F`)
  backwards.** real-regex 2026.7.37 fixed ASCII-mode `\s` to exclude them (matching Python `re`'s own
  `re.ASCII` contract; only text-mode `\s` includes them). Any SciLex grammar with a `\s`/`\S` rule
  pinned to `real::flags::ascii` — every DFA-compat grammar, via `plain()`'s own convention — now
  classifies these four bytes the same way `re` does. Regression pinned by pin-flip: fails cleanly
  (non-crashing) against real-regex < 2026.7.37, passes against 2026.7.37+.

### Changed
- **Build requires `real-regex >= 2026.7.37`** (was `>= 2026.7.25`), for the fix above.

## 2026.7.3

### Added
- **Every example grammar now DFA-accelerates (3–27×).** Bumping to real-regex 2026.7.25 (below) makes
  `lisp`, `yaml` and `python`'s default modes DFA-representable — previously they fell back to Pike. With the
  SCILEX-1 example fixes, all nine example grammars now accelerate on `dfa_modes` (dense grammars ~15–27×,
  sparser ones 3–5×; full-set geomean ~13×). See BENCHMARKS.md, refreshed with a full environment stamp. The
  transparent Pike fallback is unchanged and covered by the `dfa_modes_fallback_on_assertion` test.

### Fixed
- **Lexer mode sets are a `std::vector<std::string>`, not a `std::unordered_set`.** The unordered-set
  instantiation drifted symbols across the libc++ ABI boundary (the binding is abi3); an ordered vector is
  symbol-stable, a mode set is tiny so membership cost is unchanged, and the token stream is identical.

### Changed
- **Build requires `real-regex >= 2026.7.25`** (was `>= 2026.7.5`), tracking the current REAL release —
  the linear POSIX grammars, bounded lookarounds, Unicode `\w \d \s`, and the per-operation ReDoS-safety
  guarantee the lexer builds on.

## 2026.6.6

### Added
- **DFA fast path (opt-in).** `lexer(rules, insignificant_modes={}, dfa_modes={})` plus
  `dfa_modes_active()`; the Python `Lexer(rules, insignificant_modes=(), dfa_modes=())`
  with `dfa_modes` / `dfa_modes_active` properties. A named mode is accelerated by a
  `real::dfa` — one maximal-munch pass replacing the per-rule dispatch (a large speed-up on the
  full token path for DFA-able modes; see BENCHMARKS.md for current, stamped per-grammar figures).
  Additive and invisible: the Pike engine stays the
  floor, a mode whose rules need an assertion no DFA can represent or whose DFA fails a
  build-time audit (a lazy quantifier) silently falls back to Pike, and the token stream
  is byte-identical either way. The `sql` and `css` example grammars opt in.

*(Entries for 2026.6.7 – 2026.7.2 were not kept here per-release; those git tags are the record. Per-release
entries resume above.)*

## 2026.6.5

The contextual-lexing release: **modes** and **Layout Awareness**. Ambitious but
honest — here is what it does, and what is deliberately left for Level B.

### Added
- **Modes (contextual lexing).** A rule may carry `in_mode` (the modes it is active
  in) and an `action` (push / pop / set the per-scan mode stack), so the same byte
  lexes differently by context. Maximal munch and the exact first-byte dispatch run
  *per mode*; the lexer stays immutable and shareable (the stack is per-scan).
- **Layout Awareness (Level A).** Each token carries the mode it was lexed in; a mode
  can be marked *insignificant*, and the layout pass then passes its tokens through
  without shaping indentation.
- **Three modal example grammars** — Python f-strings (five modes), XML
  (content ↔ tag), YAML (block ↔ flow) — nine examples in all, each fuzzed by the
  oracle against an independent brute-force reference.

### Lifted (by Layout Awareness Level A)
- YAML multi-line flow collections no longer pick up spurious INDENT/DEDENT.
- Python implicit line continuation inside `()` `[]` `{}` is no longer read as a new
  block.

### Known limitations (Level B)
- **Block scalars** (`|` / `>`) and **heredocs** need a reference indent carried in
  the mode frame — that is **Layout Awareness Level B**, designed but not built.
  (Multi-line flow collections and implicit line continuation are *not* limitations
  — Layout Awareness Level A lifts both, this release.)

### Not yet
- A compile-time `static_lexer` (on REAL's `static_regex`) — grows in on demand.

### API
- `scilex::rule` gains `in_mode` (a set of mode names) and `action` (a `mode_action`:
  push / pop / set); a plain `{kind, pattern, skip}` rule is unchanged.
- `scilex::lexer(rules, insignificant_modes = {})`; accessors `mode_significant()`
  and `mode_name(id)`; `scilex::layout(tokens, mode_significant = {})` (mode-aware).
- `scilex::token` gains `mode_id` (default 0; existing aggregate inits compile
  unchanged).
- Python: `Lexer(rules, insignificant_modes=())`, `Lexer.layout(tokens)`,
  `Token.mode` (part of `==` / `hash`), `Layout(insignificant_modes)` and
  `layout(tokens, insignificant_modes)`. The low-level token tuple is now 6-field
  (`kind, lexeme, offset, line, column, mode`).
- Two invariants: an empty significance policy is byte-for-byte the positional pass
  (zero cost); the **mode** is the single source of the policy — no per-rule flag.

## 2026.6.4

Baseline before modes: the exact first-byte dispatch (REAL's first-byte API, 3–7×
over the prior textual heuristic), the zero-copy / GIL-releasing Python binding
(str + bytes, `py.typed`), the `scilex` CLI, the fuzz oracle, and seven example
grammars. See the git history for earlier calendar versions.
