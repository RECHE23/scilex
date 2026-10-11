# Changelog

All notable changes to SciLex. Versions are calendar-based (`YYYY.M.PATCH`, the
patch resetting each month; PEP 440 drops leading zeros). The project holds the
SciLang-stack gate: 100%-4D coverage of `include/`, dual-compiler, sanitizers, and a
fuzz oracle.

## Unreleased

### Fixed
- **The Python example grammar lexes as CPython's tokenizer does.** Over the 1 849 files of the standard
  library it agreed with `tokenize` on 825 (audit, 2026-10-11): `obj._x` lexed `._` as a number, `!` and `;`
  were no tokens (so `f"{x!r}"` stopped the lexer), triple-quoted and raw f-strings were not read, a format
  spec (`f"{x:>{w}}"`) lexed as code, and `\N{…}`, a backslash-newline inside a string and a byte-order mark
  stopped it. It now agrees on 1 848 with Unicode identifiers (the last file is deliberately invalid) and
  1 842 with the default ASCII ones. Thirteen modes; building the lexer still takes ~17 ms. Also new: t-strings
  (3.14), and Python's identifier definition (`XID_Start`, `XID_Continue`) in the Unicode variant.
- **`make python-oracle` checks it**, in the gate and in CI: both variants against `tokenize` over the
  running interpreter's standard library, with a floor one file below today's count.

## 2026.10.5 — 2026-10-11

### Added
- **A rule can depend on the token before it** (`rule::after`, `rule::not_after`): active only right after a
  token of the kinds listed, or everywhere else — a `/` that divides after an operand and opens a regular
  expression literal elsewhere, as JavaScript lexes it, without a mode per context. The previous token is the
  last one emitted in any mode (a skipped match is none, an error run is one; `end_of_input` before the first).
  The lexer gives a mode one DFA per distinct set of rules the previous token leaves active, so it costs
  nothing per token, and the stream, the lazy scan and `tokenize` agree as before. Python: two optional fields
  of a rule tuple, `(…, action, after, not_after)`; `.lex`: `after=A,B` and `not_after=A,B` name rules (`^` for
  the start). The oracle fuzzer draws them in its random grammars, against a reference that states the rule
  directly.
- **`%insignificant m1,m2` in a `.lex` grammar** names the modes whose tokens carry no layout structure, which
  the lexer built from it takes as its `insignificant_modes` (the CLI's `--layout` too). C++:
  `grammar::insignificant_modes`; Python: `Grammar.insignificant_modes`, which `Grammar.lexer()` uses unless
  given its own. Refused at its line and column: an unknown directive, an empty or malformed mode list, a mode
  no rule is active in.
- **Example grammars for three scientific languages**, built in (`scilex --example fortran|julia|matlab`),
  checked by the oracle fuzzer and the stream tests like the nine others, each with a DFA in every mode (a rule
  that needs a lookaround or a Unicode class stays on Pike beside it):
  - **MATLAB / Octave**: an apostrophe is the transpose after an operand and opens a character array
    anywhere else (a one-byte lookbehind, as MATLAB's lexer decides it); `%{ … %}` block comments nest;
    `...` continues a line; `2.^x` is `2 .^ x`.
  - **Fortran** (free form): `1.eq.2` is a number, an operator and a number while `1.e5` is one number;
    keywords, dotted operators and `.TRUE.` in any case; `&` continuations with a comment after the `&`;
    kind suffixes and BOZ literals.
  - **Julia**: `#= … =#` comments nest; `$name` and `$(expr)` interpolate, the expression holding
    parentheses and strings of its own (a mode per parenthesis tells the closing one apart); Unicode
    identifiers and operators; the adjoint `'` beside character literals, the range `:` beside symbols.

### Fixed
- **Lexing a whole text on the DFA is 5–11 % faster again** (`tokenize`, `scan`; arm64, Apple clang 16, `-O2`:
  `json` 130 → 143 MB/s, `css` 127 → 142, `math` 75 → 81). Since 2026.10.4 every munch also asked the DFA
  whether more text could change it, which only a stream needs; a scan over a whole text now asks REAL for the
  match alone, and the stream keeps its question.

### Changed
- **Requires real-regex 2026.10.11** (was 2026.10.10): its fixed-width and case-insensitive prefilters, and the
  `left_context` it gives every binding. Lexing throughput is unchanged by it (every row within 3 %, alternated).
- **A `.lex` line whose first non-blank character is `%` is a directive**, so a rule can no longer be named
  with a leading `%` (an unknown directive is refused, naming the line).

## 2026.10.4 — 2026-10-11

### Changed
- **SciForge v2026.10.2**: CI pins and the `sciforge-build` floor (was 2026.6.x), so an isolated wheel
  build compiles against the hardened binding headers.
- **Build requires `real-regex >= 2026.10.10`** (was `>= 2026.10.9`); the FetchContent tag and CI move with it.
  2026.10.10 gives `real::dfa::munch`, which a stream reads its gate from, and fixes `can_extend` with a
  lookaround that reads past it, which a stream relies on.
- **A stream runs at 1.4–1.6× `tokenize`'s time** (1 MiB, 4 KiB chunks, arm64: `json` 2.2 → 76 MB/s,
  `python` 0.5 → 78, `cpp` 0.6 → 76; it was 55–264× slower). Before each munch it asked every rule of the
  mode whether more text could change its match (`real::regex::can_extend`, a Pike run). The mode's DFA now
  tells it from the munch it walks anyway (`real::dfa::munch`, real-regex 2026.10.10), and `can_extend` is
  asked only of the rules left on Pike that can start with the byte there.
- **A stream's cost no longer grows with the depth of the mode stack.** Each step copied the whole stack, to
  restore it if the step had to wait for more text; it now notes what each transition changes and undoes that.
  32 000 nested pushes then pops (64 001 tokens): 572 ms → 2.2 ms, as `tokenize` (2.3 ms).
- **The `scilex` CLI escapes lexemes**, so that each output line is one token: a backslash, tab, line feed
  and carriage return print as `\\`, `\t`, `\n`, `\r`, any other control byte as `\xHH`. A lexeme holding a
  line break or a tab used to break the line-per-token format.
- **`scilex --example python` measures indentation as CPython does** (`tab_policy::python`): a tab
  advances to the next multiple of 8, and ambiguous tabs and spaces are refused. It counted a tab as one
  column, so `\t` and 8 spaces opened a block.

- **The `python` example grammar reads string prefixes and explicit line joining.** `r"…"`, `b'…'`, `u"…"`,
  `br`/`rb` (triple-quoted too) are one string token, `rf"…"`/`fr"…"` open an f-string, and a `\` that ends
  a line continues the logical line (a sixth mode, `joined`, layout-insignificant like `bracket`), where it
  was a lexical error. `joined` stays off the DFA, as the line it serves is rare: building the lexer takes
  16.3 ms instead of 13.1 (arm64, Apple clang 16, `-O2`, best of 9), lexing the sample the same.

### Fixed
- **A rule's kind must be an int that SciLex does not reserve.** A kind from `scilex::end_of_input` to
  `scilex::error` is refused when the lexer is built (`std::invalid_argument`; Python: `scilex.error`): its
  tokens read as an end of input, a layout token or an error run. Python refuses a kind that is not an
  integer (`TypeError`), where `int()` turned `1.7` into 1 and `"3"` into 3.
- **`scilex.Token` checks its arguments**: a lexeme that is not `str` or `bytes`, or a position that is not
  a `Position`, is a `TypeError` when the token is made. `Token(1, 'x', 'notpos')` was accepted, then its
  `repr` raised `SystemError`, and a token with an unhashable lexeme raised `scilex.error` from `hash`.
- **A stream counts columns as `tokenize` does when a token ends inside a code point** (a byte rule such
  as `\C`, under `column_unit::codepoints` or `utf16`). A continuation byte is scored by its lead and a lead
  by its whole sequence, and the stream scored them with the sequence not yet fed or its lead already
  dropped. It now holds back a code point it has not finished (`buffered()` counts it) and keeps 3 bytes
  before the next token.
- **A stream no longer loses the tokens before an error.** An error found in a `feed` or `finish` that had
  decided tokens before it dropped them, so what a stream gave depended on where the text was cut
  (`"abc 12 de @ x"` in one piece: nothing, then the error). The `lex_error` now carries them
  (`lex_error::decided()`; Python: `LexError.tokens`): the tokens of the earlier calls and these are every
  token before the error, wherever the text was cut.
- **An error run ends where a rule with no first byte matches.** Under `error_policy::token`, a run of
  unlexable bytes ends at the first position where a rule matches with some length, but only the rules
  filed by first byte were asked: `(?m)^[ \t]*` (skip) let the run `"!\n  "` swallow the next line's blanks.
  The fuzz oracle matches in the whole text, as the lexer does since 2026.10.2.
- **A `push=` or `set=` into a mode no rule is active in is refused at its line and column**
  (`my.lex:2:5: no rule is active in mode 'strr' (give one in=strr)`). The lexer refused it when built,
  with no line, so a typo in a mode name surfaced as a bare `scilex.error`.
- **A byte order mark that starts a `.lex` file is dropped**; it became part of the first rule's name.
- **The CLI reports an indentation error at its position** (`layout error at 3:9: …`), and its usage
  lists the `.lex` options (`in=`, `push=`, `set=`, `pop`), not only `skip`.

## 2026.10.3 — 2026-10-10

### Fixed
- **A rule that reads before its token (a leading `^` or `\A`, a multiline `^`) stays off the DFA.** Since
  2026.10.2 such a rule matches on Pike in the whole source, so a leading `^` holds only at the start of the
  text, while the default lexer's DFA matched from each token's start, where it always holds: `^#![^\n]*`
  read as a shebang in the middle of a file. The default lexer and the per-rule path give the same tokens
  again; no example grammar has such a rule, so none moves to Pike.

## 2026.10.2 — 2026-10-10

### Changed
- **Build requires `real-regex >= 2026.10.9`** (was `>= 2026.10.7`); the FetchContent tag and CI move with it.
  2026.10.9 gives each rule's left context (`real::regex::left_context`), which a stream now keeps.
- **`BENCHMARKS.md` is re-stamped** against real-regex 2026.10.9 on both ISAs (2026-10-09, arm64 on AC power
  and x86-64): Pike rows 7–64 % faster than at the 2026.9.7 stamp and `json` 8.14 → 44.75 MB/s, all of it
  REAL's (this release's SciLex and the one before its left-context change read the same against the same
  REAL); the default DFA 80–151 MB/s, 3.2–17× Pike on arm64 and 4.5–25× on x86-64; the Python binding
  2.15× `re` on the benign case.

### Fixed
- **A rule reads the text before its token.** Each rule matched the source from the token's start, so a
  lookbehind never saw what precedes it and `\b` read a token start as a word edge: `(?<=@)\d+` never matched
  after `@`, and `\b\d+` split `ab12`. Rules now match anchored at the offset in the whole source, as
  `match(text, pos)` does, in `tokenize`, in a stream (which keeps the bytes a rule may read before its
  cursor) and in error recovery (a run before a token is that token's context).
- **What follows a closing bracket on its line continues the logical line.** Python's `x = f(\n a,\n b) + 1`
  emitted NEWLINE and INDENT before the `+`: the tokens of an insignificant mode did not advance the line
  the layout pass had reached.
- **The CLI's `--errors` and `--columns` keep an example grammar's modes and DFA.** They rebuilt the lexer
  from the bare rules, so YAML's flow collections and Python's brackets emitted INDENT and DEDENT.
- **A `.lex` pattern lost to the line's trailing blanks is named.** The parser drops the blanks that end a
  line, so a pattern of spaces read as a missing field; the error now says so and how to write one (`[ ]`).
- **Python: a stream lexes a piece of 4 KB or more with the GIL released**, as `tokenize` does, and a second
  thread reaching the same stream meanwhile gets `RuntimeError` instead of racing on its buffer.

### Documentation
- The streaming guide states what a token open across many pieces costs (read again on each feed).

### Internal
- CI runs `make example` (the CLI's self-checks and the README's code), `symbol-hygiene` and `version-check`.

## 2026.10.1 — 2026-10-09

### Changed
- **Build requires `real-regex >= 2026.10.7`** (was `>= 2026.10.6`); the FetchContent tag and CI move with it.
  A rule ending on a lookaround over a Unicode class (`\w+(?=\()`, `\w+(?!\w)`) is tested in place instead of
  through REAL's lookaround sub-simulation: such a grammar lexes 6.5 % faster end to end over mixed-script text
  (178 663 tokens, the CLI, arm64), with every token stream identical to 2026.10.6's (126 lexes over the example
  grammars, the benchmark inputs and off-grammar text, with and without error recovery). The example grammars'
  default DFA path counts exactly the same instructions (g++ 14 `-O2`, xml, python, yaml, cpp).

### Fixed
- **A lexing error's context no longer cuts a character in two.** `.context` (and the message) fenced the
  offending byte, so an error on a multibyte character showed its lead byte alone and the rest as `�`:
  `ab ‹�›�рив�` for `ab привет`. The fence now holds the whole character when it is valid UTF-8, one byte
  otherwise, and the window around it starts and ends on character boundaries: `ab ‹п›риве`.

## 2026.10.0 — 2026-10-08

### Changed
- **Build requires `real-regex >= 2026.10.6`** (was `>= 2026.9.9`); the FetchContent tag and CI move with it.
  Against 2026.9.9, on arm64 (Apple clang 16 `-O2`, three alternated runs, minimum), with every token stream
  identical (124 lexes over the example grammars, the benchmark inputs and 650 000 tokens of off-grammar text,
  with and without error recovery, layout and code-point columns):
  - **Pike lexers are faster**: json 8.6 → 44.2 MB/s, math −33 %, python −20 %, css −15 %, lisp −14 %, yaml
    −6 %, cpp, sql and xml within 1 %; a modal lexer −22 %, a mono-mode one −28 %.
  - **The default DFA path holds**: every grammar within 2 % but yaml +4.5 % at 1.0 % FEWER instructions
    (g++ 14 `-O2`, arm64; json and cpp −1.0 % too), so placement; DFAs build within 2.7 %.
  - **A lexer holds what it did**: 1–2 % more live heap than with 2026.9.9 (python 535 → 542 KiB, cpp 208 → 211
    KiB, counted at `operator new`), none of it after destruction. 2026.10.0 alone had added 20–30 %.
  - A rule that alternates 64 literals or more is a trie in REAL's byte program, so a lexer whose keyword rule
    lists 2 000 words builds and lexes 256 KiB in 7.3 % fewer instructions (x86-64, measured at 2026.10.0).
  `BENCHMARKS.md` stays stamped against 2026.9.7: its tables were not re-measured.

## 2026.9.2 — 2026-09-28

### Changed
- **Build requires `real-regex >= 2026.9.9`** (was `>= 2026.9.7`). 2026.9.8 carries
  `basic_regex::can_extend`, which the stream asks, and leases each thread its own lazy DFAs, so threads
  sharing a lexer no longer queue on a Pike rule's lock; 2026.9.9 builds a `real::dfa` about three times
  faster, so a lexer's constructor takes about half as long: the Python grammar ~13.5 ms instead of ~26,
  the example grammars 0.06–2.9 ms instead of 0.07–5.6 (arm64, `-O2`, minimum of 7). The FetchContent tag
  and CI move with it. `BENCHMARKS.md` stays stamped against 2026.9.7: its tables were not re-measured.
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
- **Text fed in pieces: `lexer::stream()` and `Lexer.stream()`.** A `scilex::token_stream` (Python
  `scilex.TokenStream`) takes the text as it arrives — `feed(chunk)`, then `finish()` — and returns
  exactly the tokens `tokenize` gives the whole text, with the same munches, modes, positions and
  errors, wherever the pieces are cut. A token comes out once no text still to come can change it,
  which REAL's `can_extend` answers per rule; the stream keeps only the text from the first token it
  has not returned, so a 72 KB JSON document fed in 64-byte pieces never holds more than 128 bytes.
  Every example grammar is checked against `tokenize` for every cut in two and for random pieces. A
  Python stream takes `str` or `bytes` pieces (a `bytes` piece may end inside a UTF-8 sequence), and
  its errors carry `.position` and the bytes around it. See Guide → Streaming.
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
- **A documentation site** (<https://reche23.github.io/scilex/>), built on real-regex's Sphinx toolchain:
  guides (grammar format, modes, layout, errors, positions, performance, threads, streaming), a
  tutorial, and the C++ and Python references, its Python examples run as doctests by the local gate.

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
- `.lex`: a comma in a `push=` or `set=` mode name is refused as such, where it read as an empty mode
  name.
- `scilex/grammar.hpp` compiles without warnings under `-Wshadow` (a caught `real::regex_error` named
  `error` hid `scilex::error`); the tests now build with `-Wshadow`, the spelling of MSVC's `/W4`
  hiding warnings, so the local gate sees them before CI does.
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
