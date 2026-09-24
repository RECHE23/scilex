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
(`clang-1600.0.26.6`), `-O2`, against **real-regex 2026.9.7** (its `main` at `dcaafeb`: the release
plus the unreleased munch-memo work), on **2026-09-24**, **on AC power**, unless a section names
another stamp. C++ cases are the **best of 9** timed passes per run, reported as the **median of
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
does: **1.70× faster** (0.881 ms against 1.459 ms, the median of 3 runs; the three read 1.30×, 1.70×
and 1.87×). Since DFA acceleration became automatic, the ordinary rule set of that case runs on a
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
| benign token soup | **SciLex** (1.3–1.9×) | the rule set runs on a DFA by default, one table step per byte |
| adversarial / ReDoS | **SciLex** (linear vs exponential) | REAL is linear-time and ReDoS-safe; `re` backtracks catastrophically |
| untrusted / machine-generated | **SciLex** | no rule backtracks, so no input is exponential; the worst case is quadratic, only through a rule left on Pike, not a cliff |
| a fixed grammar compiled ahead of time | **flex** (~5–7× the binding) | a code-generated scanner; SciLex keeps the grammar as runtime data |

## C++ engine throughput — per grammar, on Pike

`bench.py` (below) measures the **Python binding**; this section measures the **C++ engine
directly** — the speed a C++ embedder or a SciParse parser sees, with no interpreter in the path.
`make bench-lex` lexes each of the nine example grammars (`examples/<lang>.hpp`) over its own sample
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
  band.

| ASCII-pinned grammar | rules | tokens | eager MB/s | lazy MB/s |
| --- | ---: | ---: | ---: | ---: |
| json | 12 | 58 793  | 8.14 | 8.43 |
| cpp  | 41 | 52 228  | 10.43 | 10.77 |
| sql  | 39 | 38 760  | 10.06 | 10.30 |
| css  | 17 | 64 224  | 10.16 | 10.63 |
| lisp |  8 | 96 600  | 13.89 | 15.36 |
| math | 12 | 123 376 | 12.04 | 13.16 |

| Unicode text-mode grammar | rules | tokens | eager MB/s | lazy MB/s |
| --- | ---: | ---: | ---: | ---: |
| xml    | 12 | 65 588 | 11.98 | 12.64 |
| yaml   | 14 | 56 829 | 7.37 | 7.54 |
| python | 65 | 53 960 | 10.62 | 11.04 |

IQR within 0.5 MB/s on every row. Three grammars are **modal** (contextual lexing): `python`
(f-strings — five modes), `xml` (content ↔ tag), `yaml` (block ↔ flow); they sit in the same band as
the flat grammars, because the dispatch runs *per mode*. `python` carries **65** rules (35 keywords +
the modal machinery) and the first-byte dispatch keeps it rule-count-independent.

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
| 64  | 10.68 |
| 128 | 10.48 |
| 256 | 10.49 |
| 512 | 10.34 |

Flat MB/s means time scales **linearly** with input on this grammar, whose rules stop scanning near
their tokens. It is not a bound for every grammar: two rules (`a*b` and `a` on `aaa…`) kept on Pike
reach the quadratic worst case described in `docs/spec.dox` (on the DFA, whose walks are memoized,
the same pair is linear).

**Reading — modes & Layout Awareness.** Contextual lexing is throughput-neutral by construction.
`make bench-lex` contrasts the modal `python` grammar with a mono-mode baseline — the same rules with
the f-string modes stripped — on the same sample: **modal 10.64 vs mono-mode 10.37 MB/s**, within
noise of each other, while the modal path does materially more work (53 960 vs 44 872 tokens — full
f-string structure, not an opaque string). Layout Awareness reads each token's mode but adds nothing
when no mode is insignificant.

## DFA — what a caller gets by default: every example grammar runs wholly on it, 5.7–18×

Every lexer tries a `real::dfa` in each mode (`dfa_policy::automatic`, the default). A rule joins its
mode's DFA when its `match()` is its longest match and it needs no assertion the DFA cannot represent;
the others (a `\b`, a `$`, a lookaround, a Unicode `\w`, a lazy delimiter) stay on Pike beside it,
and the munch merges the two with a byte-identical token stream. On the full token path
(`tokenize`), the default lexer against Pike alone:

| grammar | DFA modes | rules on Pike | Pike MB/s | DFA MB/s | speed-up | DFA build |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| yaml     | 2 | 0 |  7.38 | 133.36 | **18.1×** | 2.16 ms |
| json     | 1 | 0 |  8.16 | 126.06 | **15.4×** | 0.57 ms |
| sql      | 1 | 0 | 10.11 | 133.91 | **13.1×** | 2.60 ms |
| cpp      | 1 | 0 | 10.39 | 126.40 | **12.3×** | 5.79 ms |
| css      | 1 | 0 | 10.28 | 128.17 | **12.3×** | 2.33 ms |
| python   | 5 | 0 | 10.62 | 130.98 | **12.3×** | 27.2 ms |
| xml      | 2 | 0 | 11.91 | 109.06 |  **9.2×** | 2.37 ms |
| lisp     | 1 | 0 | 14.04 |  81.51 |  **6.0×** | 0.74 ms |
| math     | 1 | 0 | 11.81 |  66.29 |  **5.7×** | 0.07 ms |
| python-unicode | 5 | 3 | 10.40 | 34.69 | **3.3×** | 26.7 ms |

The speed-up is the median of the per-run ratios; the DFA column's IQR is ~10 % (a 256 KiB pass at
130 MB/s lasts 2 ms). **Every example grammar runs wholly on the DFA, the modal ones included** —
none leaves a rule on Pike. The spread follows token density again: `math` (123 k tokens in 256 KiB)
and `lisp` (97 k) pay the per-token cost most often, `yaml`, `sql` and `json` least. The hybrid row is
`python-unicode` (`scilex --example python-unicode`), whose identifier rule is a Unicode `\w` and
stays on Pike in three modes: 3.3× even so. The DFAs are built once, in the constructor — 0.07–5.8 ms
for a one-mode grammar, ~27 ms for Python's five modes, a vigilance point only on very short inputs.

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
| DFA (json) | binary blob        | 29 952 | 6 481 | 4 174 |
| DFA (json) | invalid-UTF-8      | 32 768 | 6 492 | 4 179 |
| DFA (json) | unclosed quote     | 32 768 | 6 470 | 4 177 |
| DFA (json) | parasitic delims   | 32 768 | 6 558 | 4 245 |
| Pike (xml) | binary blob        | 16 512 | 6 578 | 4 269 |
| Pike (xml) | invalid-UTF-8      | 32 768 | 6 544 | 4 237 |
| Pike (xml) | unclosed quote     | 0 (tolerated) | — | — |
| Pike (xml) | parasitic delims   |  4 096 | 6 746 | 4 442 |

**Reading — three findings that shape a recovering lexer.**

1. **The exception throw dominates.** A `throw`+`catch` pair alone measures **~2 300 ns**, so
   throwing once per rejected byte is by itself larger than everything else combined. A
   recovering lexer must report the skip *without* throwing per byte — which `error_policy::token`
   does.
2. **Re-lexing from scratch is setup-bound, not scan-bound.** After subtracting the throw, the
   net ~4.2 µs/position is dominated by `tokenize`'s fixed per-call setup, not the byte scan —
   which is why the DFA and Pike paths measure nearly the same here. A recovery that reuses one
   cursor avoids this; these figures are an upper bound.
3. **A non-fail-fast rule is O(remaining) per position.** A rule like `[^!]*!` (a maximal run
   before a terminator) that never completes scans to the end of the input before failing, so
   every recovery position re-scans what's left — **~490 000 ns/position** on an 8 KiB
   no-terminator run, and quadratic in the input. The previous stamp read ~451 000 and the one
   before ~199 000; the earlier gap was traced to the host, not the engine (two REAL versions twenty
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
| `scilex.Lexer.tokenize` | 0.881 ms | **1.70× faster** |
| `re.finditer` (master pattern) | 1.459 ms | 1.0× (baseline) |

**Reading.** The three runs read 1.30×, 1.70× and 1.87× — `re` held within 3 % across them, SciLex
varied more (1.094, 0.881, 0.810 ms), so the ratio is quoted as a range. The previous stamp read
1.39× with the rule set on Pike; this lexer's rules now run on a DFA by default. One case is one
case, and ReDoS-safety remains the reason to choose SciLex. For multi-threaded throughput, `tokenize`
releases the GIL around the scan of inputs ≥ 4 KB; the lazy `scan` holds the GIL per one-token step
(the parser-friendly path, not the throughput path).

### Pathological input (ReDoS — SciLex wins decisively)

The classic ReDoS trigger `(a+)+b` over a run of `n` `a`s with no terminating `b`. A
backtracking engine explores `O(2ⁿ)` partitions; REAL never backtracks, and on this input SciLex
scales linearly.

| n | `scilex` (linear) | `re.match` (backtracking) |
| ---: | ---: | ---: |
| 16 | ~0.45 µs | ~2.1 ms |
| 18 | ~0.45 µs | ~8.8 ms |
| 20 | ~0.46 µs | ~34.7 ms |
| 22 | ~0.47 µs | ~137.6 ms |
| 24 | ~0.47 µs | ~570 ms |
| 26 | ~0.50 µs | ~2.30 s |
| 1000 | ~4.7 µs | would not finish |

**Reading.** `re`'s time roughly **quadruples every +2** in `n` (exponential); SciLex
grows **linearly** and is still ~4.7 µs at `n = 1000`, where `re` would not finish in any
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
0.98 ms to 0.67 ms — flat, the variation being run-to-run noise.

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
| **scilex** `tokenize()` | 23.2 | 32.4 | a token stream as Python objects |
| **scilex** `scan()` (lazy) | 16.6 | 22.5 | the same, lazily (fully consumed here) |
| Pygments | 9.9 | 0.7 | pure-Python styled (type, text) pairs — a highlighting superset |
| tree-sitter | 11.9 | 9.9 | a full parse tree in C, returned as a handle |
| flex (codegen) | 161.0 | — | a compile-time DFA scanner (C), best-of-30 internal |

The previous stamp read SciLex at 5.4 and 6.9 MB/s here, behind tree-sitter and, on JSON, Pygments;
the difference is the DFA, which the binding's lexers now get by default.

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
  It is ~5–7× the SciLex binding, which is the honest shape of that trade.
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
