# SciLex — performance baseline

A reproducible baseline at two layers: the **C++ engine** directly, per grammar
(`make bench-lex`), and the **Python binding** against the standard-library `re`
(`make bench` runs the C++ table and then `bench.py`). Its purpose is twofold: a
**regression tripwire** between versions on the same machine, and an honest statement
of **where SciLex wins and where it does not**.

Both are informational only — they print tables, are never invoked by
`full-local-gate`, and never fail a build on a number. `make bench-lex` does fail on one thing: a
DFA row whose token stream differs from its Pike row's, because a speed-up of a different answer
measures nothing (see the retraction under the DFA table). `make bench-lex` needs no Python build.

For *why* the numbers look the way they do — the linear-scan engine and the REAL
foundation — see the [design tour](https://github.com/RECHE23/scilex/blob/main/docs/design.dox).

## Measurement stamp

Every table below was measured on **Apple M1 Pro** (arm64, 8 cores), **Apple LLVM 16.0.0**
(`clang-1600.0.26.6`), `-O2`, against **real-regex 2026.10.11**, SciLex at `8cb39fe`, on
**2026-10-11**, **on AC power**, unless a section names another stamp. C++ cases are the **best of 9** timed passes per run, reported as the **median of
N = 6 runs** (IQR quoted where it matters); the binding rows are the median of 3 runs. The durable
content is the **ratios**; absolute MB/s track the host and are meaningful only under this stamp —
never a bare number. Reproduce with `make bench-lex` and `make bench`.

**"On AC power" is part of the stamp because it had to be learned.** This machine throttles hard on
battery: the same `json` grammar against the same real-regex once read **7.00 MB/s on battery against
9.02 on AC**, a 29 % gap with nothing else changed, and a refresh measured on battery would have
published a regression that did not exist. An A/B is immune (both sides throttle together); an
ABSOLUTE table is not.

## The honest headline

SciLex was **not** built to beat `re` on raw throughput, and on the benign case measured below it
does: **2.09× faster** (0.665 ms against 1.388 ms, the median of 3 runs; the three read 2.14×, 2.08×
and 2.09×). Since DFA acceleration became automatic, the ordinary rule set of that case runs on a
DFA without the caller asking. Read that narrowly: it is one case, 4000 tokens over ~10 KB of
identifier-operator soup.

What SciLex guarantees is **ReDoS-safety by construction**: no rule can make the scanner backtrack,
so no input is exponential. It is not a linear bound on every input — the worst case is quadratic,
through a rule left on Pike that scans far and loses at every position (see `docs/spec.dox`); the
rules on a DFA are memoized and linear together. On an adversarial pattern, `re` degrades
exponentially while SciLex stays flat — the difference that matters for a lexer fed untrusted or
machine-generated input.

### Gains and losses at a glance

| input | winner | why |
| --- | --- | --- |
| benign token soup | **SciLex** (2.1×) | the rule set runs on a DFA by default, one table step per byte |
| adversarial / ReDoS | **SciLex** (linear vs exponential) | REAL is linear-time and ReDoS-safe; `re` backtracks catastrophically |
| untrusted / machine-generated | **SciLex** | no rule backtracks, so no input is exponential; the worst case is quadratic, only through a rule left on Pike, not a cliff |
| a fixed grammar compiled ahead of time | **flex** (~6× the binding) | a code-generated scanner; SciLex keeps the grammar as runtime data |

## C++ engine throughput — per grammar, on Pike

`bench.py` (below) measures the **Python binding**; this section measures the **C++ engine
directly** — the speed a C++ embedder or a SciParse parser sees, with no interpreter in the path.
`make bench-lex` lexes each example grammar (`examples/<lang>.hpp`) over its own sample
scaled to a ~256 KiB steady-state input, reporting MB/s for `tokenize()` (eager, full token vector)
and `scan()` (lazy — the parser path). The rows here are the **Pike engine** alone (the per-rule scan
+ first-byte dispatch, `dfa_policy::requested` with no mode): what a rule the DFA cannot take costs,
and the baseline the DFA table divides by. What a caller gets by default is the DFA table below.

The engine has **two regimes**, reported separately because they run different match-time
machinery:

- **ASCII-pinned grammars** — their `\s`/`\w`/`\d` are pinned to byte-level classes (an explicit
  `(?a)` flag), so each is a 256-bit membership test.
- **Unicode text-mode grammars** — they keep the default Unicode shorthands, which compile to
  *code-point predicates* (decode a code point, then test membership). Both regimes sit in the same
  band, `json` apart: its Pike row reads two to five times the others' since REAL 2026.10 (below).

| ASCII-pinned grammar | rules | tokens | eager MB/s | lazy MB/s |
| --- | ---: | ---: | ---: | ---: |
| json    | 12 | 58 793  | 43.94 | 48.08 |
| cpp     | 41 | 52 228  | 11.09 | 11.34 |
| sql     | 39 | 38 760  | 10.84 | 11.15 |
| css     | 17 | 64 224  | 12.70 | 13.02 |
| lisp    |  8 | 96 600  | 17.52 | 18.89 |
| math    | 12 | 123 376 | 19.77 | 21.33 |
| fortran | 18 | 75 240  | 10.69 | 11.14 |
| matlab  | 23 | 75 852  |  8.04 |  8.30 |

| Unicode text-mode grammar | rules | tokens | eager MB/s | lazy MB/s |
| --- | ---: | ---: | ---: | ---: |
| xml    | 12 | 65 588 | 12.63 | 13.23 |
| yaml   | 14 | 56 829 |  8.11 |  8.33 |
| python | 69 | 53 960 | 12.72 | 13.16 |
| julia  | 35 | 84 664 |  8.61 |  8.90 |

IQR within 0.4 MB/s on every row. Against the previous stamp (real-regex 2026.10.9) the eight rows both
stamps measure read within 2.5 % of their old value, `json` 44.75 → 43.94 MB/s the widest of the
ASCII-pinned ones; `python` fell 10 % (14.16 → 12.72) because its grammar grew: string prefixes and
explicit line joining, 69 rules and a sixth mode. The predicate on the previous token, new in this
release, costs about five instructions per token (callgrind, x86-64: `math` +1.3 % Ir, `json` −0.03 %),
below what these rows can resolve. The grammars are **modal** (contextual lexing) where the language
asks for it: `python` (f-strings, brackets, joined lines — six modes), `xml` (content ↔ tag), `yaml`
(block ↔ flow), `matlab` (block comments), `julia` (nested comments, interpolation); they sit in the
same band as the flat grammars, because the dispatch runs *per mode*. `python` carries **69** rules (35
keywords + the modal machinery) and the first-byte dispatch keeps it rule-count-independent.

Method: lexer built once, warmup then **min of 9** timed passes per run, reported as the **median of
N = 6 runs**, `-O2`, every result consumed through a volatile sink. Sizes are KiB (1024 B),
throughput is MB/s (10⁶ B/s).

**Reading — what sets the pace.** Dispatch is **exact**: a 256-bucket first-byte index built from
REAL's first-byte API (`has_first_byte_set` / `unique_first_byte` / `may_start_with`) tries a rule at a
position **only if its pattern can begin there**. Throughput is then governed mainly by **token
density** — the cost is paid per token.

**Reading — eager vs lazy.** `scan()` edges out `tokenize()` (it never materializes the token
vector). Its memory is the source plus the mode stack while the grammar's DFA walks die near their
tokens, which is every shipped grammar's case; a walk that runs more than 32 bytes past its last
accept arms the mode's walk memo, which then holds up to one bit per source byte per DFA state
(`docs/spec.dox`). Measured with massif (valgrind 3.22, g++ 13.3, x86-64, 2026-09-24): scanning
1 MiB of JSON holds 1.46 MB at steady state (the source and the lexer); `a*b` beside `a` over 1 MiB
of `a` peaks at 1.21 MB, the armed memo ≈ 0.17 MB of it.

**Reading — linearity on a real grammar (C++).** The same `cpp` grammar over growing inputs, on Pike:

| KiB | eager MB/s |
| ---: | ---: |
| 64  | 11.08 |
| 128 | 11.08 |
| 256 | 11.06 |
| 512 | 10.95 |

Flat MB/s means time scales **linearly** with input on this grammar, whose rules stop scanning near
their tokens. It is not a bound for every grammar: two rules (`a*b` and `a` on `aaa…`) kept on Pike
reach the quadratic worst case described in `docs/spec.dox` (on the DFA, whose walks are memoized,
the same pair is linear).

**Reading — modes & Layout Awareness.** Contextual lexing is throughput-neutral by construction.
`make bench-lex` contrasts the modal `python` grammar with a mono-mode baseline — the same rules with
the f-string modes stripped and every other rule moved into one mode — on the same sample: **modal 12.76
vs mono-mode 10.50 MB/s**, the modal path 22 % faster while it produces 20 % more tokens (53 960 vs
44 872 — full f-string structure, not an opaque string). The flattened baseline is the slower one since
the grammar gained its joined-line mode: that mode's whitespace and line-break rules land beside the
default mode's and are tried at every position, where the modal lexer tries each only in its own mode. Layout Awareness reads each token's mode but adds nothing
when no mode is insignificant.

## DFA — what a caller gets by default: every example grammar has one in every mode, 1.6–17×

Every lexer tries a `real::dfa` in each mode (`dfa_policy::automatic`, the default). A rule joins its
mode's DFA when its `match()` is its longest match and it needs no assertion the DFA cannot represent;
the others (a `\b`, a `$`, a lookaround, a Unicode `\w`, a lazy delimiter) stay on Pike beside it,
and the munch merges the two with a byte-identical token stream. On the full token path
(`tokenize`), the default lexer against Pike alone:

| grammar | DFA modes | rules on Pike | Pike MB/s | DFA MB/s | speed-up | DFA build |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| yaml     | 2 | 0 |  8.07 | 138.99 | **17.2×** | 1.54 ms |
| sql      | 1 | 0 | 10.76 | 154.04 | **14.2×** | 1.48 ms |
| cpp      | 1 | 0 | 11.09 | 138.08 | **12.6×** | 2.97 ms |
| python   | 6 | 0 | 12.68 | 140.85 | **11.2×** | 21.5 ms |
| css      | 1 | 0 | 12.67 | 140.65 | **11.1×** | 1.54 ms |
| xml      | 2 | 0 | 12.57 | 119.37 |  **9.5×** | 2.04 ms |
| lisp     | 1 | 0 | 17.57 | 100.78 |  **5.7×** | 0.53 ms |
| math     | 1 | 0 | 19.68 |  83.27 |  **4.2×** | 0.06 ms |
| fortran  | 1 | 2 | 10.69 |  39.12 |  **3.6×** | 4.41 ms |
| json     | 1 | 0 | 43.69 | 140.88 |  **3.2×** | 0.38 ms |
| python-unicode | 6 | 4 | 12.18 | 37.02 | **3.0×** | 22.6 ms |
| matlab   | 2 | 6 |  7.95 |  22.06 |  **2.8×** | 1.91 ms |
| julia    | 6 | 25 | 8.58 |  13.86 |  **1.6×** | 6.02 ms |

The speed-up is the median of the per-run ratios; the DFA column's IQR is 1–9 % (a 256 KiB pass at
140 MB/s lasts 2 ms). Against the previous stamp the DFA column of the rows both measure moved −1.2 to +4.7 %,
and every build is the same but the two Python grammars', whose sixth mode takes them from 13.1 and 14.6
to 21.5 and 22.6 ms. **Nine example
grammars run wholly on the DFA, the modal ones included** — none leaves a rule on Pike. The spread follows
token density again: `math` (123 k tokens in 256 KiB) and `lisp` (97 k) pay the per-token cost most
often, `yaml`, `sql` and `json` least. Four rows are hybrids, a DFA in every mode with the rules it cannot
reproduce on Pike beside it: `python-unicode` (`scilex --example python-unicode`), whose identifier rule
is a Unicode `\w`; `fortran`, whose numbers stop before a dotted operator (a lookahead) and whose
preprocessor lines start a line (`^`); `matlab`, whose apostrophe is a transpose or a character array by
the byte before it (a lookbehind), whose numbers stop before an element-wise operator and whose block
comment markers hold a line alone; `julia`, whose identifiers are Unicode `XID` classes in every code mode
and whose adjoint, character and symbol rules look behind. They gain 1.6–3.6×. The DFAs are built once, in the constructor — 0.06–4.4 ms for a one-mode grammar, ~22 ms
for Python's six modes, a vigilance point only on very short inputs.

**x86-64.** The same harness on a second ISA — g++ 13.3, `-O2`, 6 cores, in a development container,
2026-10-09, SciLex at `e7f5c86` and real-regex 2026.10.9, before the Fortran, Julia and MATLAB grammars
and Python's sixth mode; each cell the best of 6 runs (in this container a
wall-clock burst lasts seconds, so the minimum across runs is the reading; here the best and the median
of each DFA cell sat within 2.3 %):

| grammar | Pike MB/s | DFA MB/s | speed-up | DFA build |
| --- | ---: | ---: | ---: | ---: |
| yaml     |  4.46 | 113.08 | **25.4×** | 2.27 ms |
| sql      |  5.66 | 135.84 | **24.0×** | 1.90 ms |
| cpp      |  5.92 | 118.22 | **20.0×** | 3.22 ms |
| css      |  6.77 | 107.53 | **15.9×** | 1.89 ms |
| python   |  7.50 | 110.86 | **14.8×** | 15.1 ms |
| xml      |  6.99 |  97.96 | **14.0×** | 2.56 ms |
| lisp     |  9.41 |  77.18 |  **8.2×** | 0.82 ms |
| math     | 10.14 |  60.96 |  **6.0×** | 0.12 ms |
| json     | 25.72 | 115.63 |  **4.5×** | 0.60 ms |
| python-unicode | 7.28 | 20.25 | **2.8×** | 15.8 ms |

The DFA column landed near the arm64 stamp of the same day (61–136 against 80–151 MB/s); the Pike column
did not — it read 42–49 % lower on this host — so the speed-ups ran higher there (4.5–25.4× against
3.2–16.9×).
A ratio is durable only within one ISA and one host; the ordering of the grammars is what both agree on.

The DFA's walks are memoized over the source (`real::dfa_munch_memo`), which makes the rules on it
linear together on every input; the memo arms itself only on a walk that runs more than 32 bytes past
its last accept, so none of the rows above pays for it.

**Retraction — the previous `xml` row.** The previous stamp (2026-08-11, real-regex 2026.8.13)
published `xml` at **249.89 MB/s, 23.6×**. That row lexed its sample wrongly: the grammar's comment and
CDATA rules were lazy then, the DFA of that REAL version took them as greedy, and one comment token ran
from the first `<!--` to the last `-->` — **76 tokens where Pike finds 262 200** on 1 MiB (re-run
2026-09-24 against those two versions). The harness counted tokens on the Pike lexer only, so the
table showed Pike's count beside the DFA's time. Three things have changed since: REAL decides per rule
whether the DFA reproduces its `match()` (a lazy rule stays on Pike), the xml rules are now written so
that they are DFA-faithful, and `make bench-lex` compares each DFA row's token stream with its Pike
row's before timing either, and refuses to report a row where they differ. The earlier rows of that
stamp measured one mode each (`dfa_policy::requested`, `default` only); these measure the default
lexer, every mode, which is what a caller runs.

## Failure-cost — the recover-and-resync loop on adversarial input

When a lexer meets bytes no rule matches — a binary blob, an invalid-UTF-8 run, an unclosed
string, parasitic punctuation — a recovering lexer must skip the offending byte and resume.
This section baselines the cost of that loop **per rejected position**, on both engine paths
(an ASCII grammar on the DFA, and a Unicode text-mode grammar on Pike). The loop is simulated over the
public `tokenize` API (recover, step one byte, re-lex), on a deterministic adversarial corpus
versioned in the harness. Two costs are separated: the raw per-position cost, and the
exception-throw cost isolated on its own (an in-lexer recovery — `error_policy::token` — does not
throw per byte), leaving a **net per-position** figure.

| path | corpus | rejected positions | raw ns/pos | net ns/pos |
| --- | --- | ---: | ---: | ---: |
| DFA (json) | binary blob        | 29 952 | 5 935 | 3 822 |
| DFA (json) | invalid-UTF-8      | 32 768 | 5 922 | 3 809 |
| DFA (json) | unclosed quote     | 32 768 | 5 923 | 3 810 |
| DFA (json) | parasitic delims   | 32 768 | 5 921 | 3 808 |
| Pike (xml) | binary blob        | 16 512 | 6 010 | 3 897 |
| Pike (xml) | invalid-UTF-8      | 32 768 | 5 938 | 3 825 |
| Pike (xml) | unclosed quote     | 0 (tolerated) | — | — |
| Pike (xml) | parasitic delims   |  4 096 | 6 167 | 4 054 |

**Reading — three findings that shape a recovering lexer.**

1. **The exception throw dominates.** A `throw`+`catch` pair alone measures **~2 100 ns**, so
   throwing once per rejected byte is by itself larger than everything else combined. A
   recovering lexer must report the skip *without* throwing per byte — which `error_policy::token`
   does.
2. **Re-lexing from scratch is setup-bound, not scan-bound.** After subtracting the throw, the
   net ~3.8 µs/position is dominated by `tokenize`'s fixed per-call setup, not the byte scan —
   which is why the DFA and Pike paths measure nearly the same here. A recovery that reuses one
   cursor avoids this; these figures are an upper bound.
3. **A non-fail-fast rule is O(remaining) per position.** A rule like `[^!]*!` (a maximal run
   before a terminator) that never completes scans to the end of the input before failing, so
   every recovery position re-scans what's left — **~468 000 ns/position** on an 8 KiB
   no-terminator run, and quadratic in the input. The four stamps before read ~460 000, ~490 000,
   ~451 000 and ~199 000; the earlier gap was traced to the host, not the engine (two REAL versions twenty
   releases apart read within 1.4 % of each other in one session). This is what a first-byte
   prefilter (`may_start_with`) mitigates by skipping positions that cannot begin the rule, and
   what the DFA's memo removes for a rule on the DFA.

## Binding baseline (versus `re`)

Measured under the stamp above (3 runs of `benchmarks/bench.py`, median reported; Python 3.14, the
abi3 extension as built by `make python`).

### Benign tokenization (the everyday case — SciLex wins, on one case)

Tokenizing ~10 KB of ordinary `ident = ident + number * ident - number ;` soup into
4000 tokens (numbers, identifiers, operators; whitespace skipped). SciLex compiles the
rule set once (a reused `Lexer`); the `re` baseline is the standard "master pattern"
tokenizer (`(?P<NUM>…)|(?P<ID>…)|…` + `finditer`).

| tokenizer | time | vs `re` |
| --- | ---: | ---: |
| `scilex.Lexer.tokenize` | 0.665 ms | **2.09× faster** |
| `re.finditer` (master pattern) | 1.388 ms | 1.0× (baseline) |

**Reading.** The three runs read 2.14×, 2.08× and 2.09× — SciLex within 0.2 % across them (0.664,
0.665, 0.665 ms), `re` within 3 % (1.381–1.424 ms). The previous stamp read 2.15× (0.656 against
1.435 ms): the SciLex side moved by 1.4 %, inside what three runs resolve. One case is one
case, and ReDoS-safety remains the reason to choose SciLex. For multi-threaded throughput, `tokenize`
releases the GIL around the scan of inputs ≥ 4 KB; the lazy `scan` holds the GIL per one-token step
(the parser-friendly path, not the throughput path).

### Pathological input (ReDoS — SciLex wins decisively)

The classic ReDoS trigger `(a+)+b` over a run of `n` `a`s with no terminating `b`. A
backtracking engine explores `O(2ⁿ)` partitions; REAL never backtracks, and on this input SciLex
scales linearly.

| n | `scilex` (linear) | `re.match` (backtracking) |
| ---: | ---: | ---: |
| 16 | ~0.41 µs | ~2.1 ms |
| 18 | ~0.42 µs | ~8.2 ms |
| 20 | ~0.44 µs | ~33.0 ms |
| 22 | ~0.45 µs | ~132.4 ms |
| 24 | ~0.45 µs | ~526 ms |
| 26 | ~0.47 µs | ~2.11 s |
| 1000 | ~4.5 µs | would not finish |

**Reading.** `re`'s time roughly **quadruples every +2** in `n` (exponential); SciLex
grows **linearly** and is still ~4.5 µs at `n = 1000`, where `re` would not finish in any
practical time. This is the case SciLex exists for.

### Rule-count scaling — the first-byte dispatch

A *realistic* lexer (a small-language rule set: whitespace, line comments, numbers,
strings, an identifier rule, operators, plus N literal keyword rules before the
identifier) over ~11 KB of representative source (3240 tokens), swept over the rule count.
A naive scanner tries **every** rule at **every** position — cost `Θ(n_rules × input)`. This
section measured how steeply that grew and then how much a **first-byte dispatch** (index
rules by their possible leading byte; try only the current byte's bucket plus the rules
without a fixed leading byte) prunes it.

| rules | before (all-rules scan) | after (first-byte dispatch) | speedup |
| ---: | ---: | ---: | ---: |
| 6  | ~7.5 ms  | ~5.7 ms | 1.3× |
| 14 | ~12.2 ms | ~5.8 ms | 2.1× |
| 22 | ~16.8 ms | ~5.9 ms | 2.8× |
| 30 | ~21.4 ms | ~5.9 ms | 3.6× |
| 38 | ~26.1 ms | ~6.0 ms | 4.4× |
| 46 | ~30.7 ms | ~6.1 ms | **5.1×** |

**The motivating data (before).** With the all-rules scan, time grew **linearly with the
rule count** — ~**578 µs per added rule**, **4.1× slower at 46 rules than at 6**. So at
realistic sizes the scan was dominated by *trying rules that cannot match the current byte*.
A static look at the 46-rule lexer confirmed it: averaged over the input, only **~1.8 of 46**
rules have a leading byte that could match a position — so a dispatch should try ~1.8 instead
of 46, i.e. **~25× fewer match attempts**.

**The result (after).** The first-byte dispatch (`lexer.hpp`: a 256-bucket index built once
at construction; only the current byte's bucket + the general rules are tried) makes
tokenization **essentially rule-count-independent**: the per-rule slope collapsed from
~578 µs to **~10 µs** (58× flatter), 46-vs-6 rules from 4.1× to **1.1×**, and the 46-rule
lexer is **~5.1× faster**. Behaviour is unchanged — a rule is bucketed only when its pattern
provably begins with one fixed literal; any class, escape, anchor, alternation, optional
lead, or compile flag sends it to the general list (tried everywhere), so the dispatch can
only ever try *more* rules than needed, never fewer. The 43 Python tests and the C++ suite
(incl. dedicated dispatch tests) pass unchanged; 100 % 4D on `lexer.hpp`.

**Verdict.** Implemented (data-backed, measured ~5× on a realistic 46-rule lexer). The
*textual* heuristic this section measured has since been replaced by REAL's **exact**
first-byte API, which buckets class, alternation, and icase leads too (not just plain
literals) — see the C++ engine table above, where it lifted the engine 3–7× and is now the
dispatch. The figures here predate that switch (they are the Python-binding study via
`bench.py`). Aho-Corasick / a fuller prefilter remain not warranted (no data demands them).
Re-run under this stamp, with the exact dispatch and the default DFA, the 6- to 46-rule lexers read
0.60 to 0.61 ms in every run — flat.

## Cross-tool comparison — other lexers on the same input

The C++ tables above are SciLex's own engine. This section places the **Python-embedded** lexer
(`scilex` the extension) beside other tokenizers on the *same* files and the *same* task — a full
tokenization pass — timed on the shared `sciforge.bench` substrate (warmed, best-of-N, 95% bootstrap
CI), under the stamp above, the median of 3 runs. The numbers are **not cherry-picked**: where a tool
beats SciLex, its number is here as measured.

Corpus: a ~515 KB JSON document and a ~512 KB block of ordinary Python source, both generated
deterministically by the harness (`benchmarks/bench_compare.py`, cached under `benchmarks/data/`).

| Tool | `big.json` (MB/s) | `sample.py` (MB/s) | What it produces |
| --- | --- | --- | --- |
| **scilex** `tokenize()` | 27.0 | 37.2 | a token stream as Python objects |
| **scilex** `scan()` (lazy) | 17.4 | 22.7 | the same, lazily (fully consumed here) |
| Pygments | 10.4 | 0.7 | pure-Python styled (type, text) pairs — a highlighting superset |
| tree-sitter | 12.8 | 10.4 | a full parse tree in C, returned as a handle |
| flex (codegen) | 162.4 | — | a compile-time DFA scanner (C), best-of-30 internal |

Pygments 2.21.0, tree-sitter 0.26.0 with `tree-sitter-json` 0.24.8 and `tree-sitter-python` 0.25.0. The
previous stamp read SciLex at 27.5 and 37.3 MB/s, within 2 % of these; the one before at 23.2 and 32.4,
and the one before that at 5.4 and 6.9, behind tree-sitter and, on JSON, Pygments — the difference then
was the DFA, which the binding's lexers get by default.

**Read this with the comparability notes — the tools do different amounts of work:**

- **SciLex's figure includes materialising a Python object per token** (the binding cost). The C++
  engine itself, without the binding, runs faster still — the DFA table above. `scan()` pays one
  Python call per token on top, which is why it trails `tokenize()`.
- **tree-sitter** builds a full parse tree in C and returns a handle; no per-token Python object is
  created (walking the tree would add that). It is also **incremental** — a capability this one-shot
  pass does not exercise, and a genuine tree-sitter win.
- **Pygments** is pure Python and produces styled pairs for highlighting; its Python lexer does
  substantially more per token.
- **flex** is the raw-throughput **ceiling**: a code-generated native DFA with a build step and a fixed
  grammar — exactly the axis SciLex does *not* compete on (grammar-as-data, ReDoS-safe, modes/layout).
  It is ~6× the SciLex binding, which is the honest shape of that trade.
- **Not measured:** Logos and re2c (Rust / a separate C-codegen toolchain) — named, not benchmarked.

**The honest reading.** Among the Python-embedded options measured, SciLex is now the fastest on both
corpora; a code generator (flex) beats everyone, and tree-sitter answers a different question (a
tree, incrementally). SciLex's case is still not this number — it is a ReDoS-safe lexer whose
grammar is runtime data, with modes, layout, and recovery, callable from C++ and Python. The
comparison confirms the positioning in the [axes page](@ref comparison), it does not overturn it.

## Methodology & reproduction

- **Goal:** a regression tripwire plus an honest win/lose map — not a throughput
  contest. Compare a fresh `make bench` to this table **on the same machine**; a clear,
  repeatable change is the signal.
- **Cross-tool:** `python3 benchmarks/bench_compare.py` (optionally `--json`). Its competitor
  dependencies are **optional** — Pygments, `tree_sitter` + the grammar packs, and `flex` + a C
  compiler are each skipped with a note if absent, never a hard failure. Figures above were taken with
  all present (`tree-sitter-json` and `tree-sitter-python` from PyPI).
- **Reproduce:** `make bench-lex` compiles and runs the C++ per-grammar table (no
  Python needed); `make bench` runs that and then builds the extension in place and runs
  `benchmarks/bench.py`. The pathological sweep stops `re` once a single match passes one
  second (its curve is already established); SciLex is measured well past that.
- **Not gated.** `make bench` is excluded from `full-local-gate` on purpose — a noisy
  wall-time measurement must never turn a clean build red.
- **Grows in:** a compile-time `static_lexer` (REAL's `static_regex`) is a known lever, grown in
  when a measured workload justifies it. No phantom numbers here for paths not yet built.
