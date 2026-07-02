# SciLex — performance baseline

A reproducible baseline at two layers: the **C++ engine** directly, per grammar
(`make bench-lex`), and the **Python binding** against the standard-library `re`
(`make bench` runs the C++ table and then `bench.py`). Its purpose is twofold: a
**regression tripwire** between versions on the same machine, and an honest statement
of **where SciLex wins and where it does not**.

Both are informational only — they print tables, are never invoked by
`full-local-gate`, and never fail a build. `make bench-lex` needs no Python build;
`make bench` runs the C++ engine table first, then the binding comparison.

For *why* the numbers look the way they do — the linear-scan engine and the REAL
foundation — see the [design tour](https://github.com/RECHE23/scilex/blob/main/docs/design.dox).

## The honest headline

SciLex is **not** built to beat `re` on raw throughput, and it does not. `re` is a
mature C backtracking engine; SciLex runs REAL's linear-time NFA at every position,
through the abi3 binding, and builds rich `Token` objects. On benign input that costs
a few times more per byte.

What SciLex guarantees instead is **linear time, ReDoS-safe by construction**: no rule
can make the scanner backtrack catastrophically. On an adversarial (or simply
unlucky) pattern, `re` degrades exponentially while SciLex stays flat — and *that* is
the difference that matters for a lexer fed untrusted or machine-generated input.

### Gains and losses at a glance

| input | winner | why |
| --- | --- | --- |
| benign token soup | `re` (~2×) | a mature C backtracking engine; SciLex runs REAL's NFA per position and builds rich `Token`s (the gap narrowed to ~2× with the zero-copy binding + exact dispatch) |
| adversarial / ReDoS | **SciLex** (linear vs exponential) | REAL is linear-time and ReDoS-safe; `re` backtracks catastrophically |
| untrusted / machine-generated | **SciLex** | the linear bound holds on *every* input — no pathological cliff |

## C++ engine throughput — per grammar

`bench.py` (below) measures the **Python binding**; this section measures the **C++
engine directly** — the speed a C++ embedder or a SciParse parser sees, with no
interpreter in the path. `make bench-lex` lexes each of the nine example grammars
(`examples/<lang>.hpp`) over its own sample scaled to a ~256 KiB steady-state input,
reporting MB/s for `tokenize()` (eager, full token vector) and `scan()` (lazy, O(1)
memory — the parser path). All rows are the **Pike engine** (the per-rule scan + first-byte
dispatch); the DFA fast path is a separate opt-in, reported on its own below.

The engine has **two regimes**, reported separately rather than as one average, because
they run different match-time machinery:

- **ASCII-pinned grammars** — their `\s`/`\w`/`\d` are pinned to byte-level classes (an
  explicit `(?a)` flag), so each is a 256-bit membership test.
- **Unicode text-mode grammars** — they keep the default Unicode shorthands, which compile
  to *code-point predicates* (decode a code point, then test membership). An earlier
  measurement put these at a **0.8–2.5 MB/s** floor when the shorthands expanded to a
  byte-automaton alternative per code-point range; with the current code-point-predicate
  path both regimes now sit in the **same 8–13 MB/s band** — the floor is gone.

| ASCII-pinned grammar | rules | tokens | eager MB/s | lazy MB/s |
| --- | ---: | ---: | ---: | ---: |
| json | 12 | 58 793  | 11.24 | 11.54 |
| cpp  | 41 | 52 228  | 11.28 | 11.77 |
| sql  | 39 | 38 760  | 11.21 | 11.58 |
| css  | 17 | 64 224  |  8.92 |  9.21 |
| lisp |  8 | 96 600  |  8.97 |  9.56 |
| math | 12 | 123 376 |  7.95 |  8.46 |

| Unicode text-mode grammar | rules | tokens | eager MB/s | lazy MB/s |
| --- | ---: | ---: | ---: | ---: |
| xml    | 12 | 65 588 | 13.27 | 14.39 |
| yaml   | 14 | 56 829 | 12.64 | 13.29 |
| python | 65 | 53 960 | 10.62 | 10.96 |

Three grammars are **modal** (contextual lexing): `python` (f-strings — five modes), `xml`
(content ↔ tag), `yaml` (block ↔ flow). They sit in the same band as the flat grammars —
the dispatch runs *per mode*, so modes cost throughput nothing structural. `python` carries
**65** rules (35 keywords + the modal machinery) yet holds ~10.6 MB/s: the first-byte
dispatch keeps it rule-count-independent.

Method: lexer built once, warmup then **min of 9** timed passes, `-O2`, every result
consumed through a volatile sink. Sizes are KiB (1024 B), throughput is MB/s (10⁶ B/s),
same machine as the conditions below. Reproduce with `make bench-lex`.

**Reading — what sets the pace.** Dispatch is **exact**: the lexer builds a 256-bucket
first-byte index from REAL's first-byte API (`has_first_byte_set` / `unique_first_byte` /
`may_start_with`), so a rule is tried at a position **only if its pattern can begin there**.
That collapses the per-position rule count, leaving throughput governed mainly by **token
density** — the cost is paid per token (one maximal-munch decision each): `math` is densest
(123 k tokens) and slowest (7.95), while the lower-density grammars top the tables (11–13).

**Reading — eager vs lazy.** `scan()` edges out `tokenize()` (it never materializes the
token vector), but only just: both pay REAL's per-position NFA scan, which dominates. The
lazy path's real win is **O(1) memory**, not speed — which is why a parser prefers it.

**Reading — linearity (the guarantee, in C++).** The same `cpp` grammar over growing
inputs:

| KiB | eager MB/s |
| ---: | ---: |
| 64  | 11.65 |
| 128 | 11.40 |
| 256 | 11.30 |
| 512 | 11.17 |

Flat MB/s means time scales **linearly** with input — the linear, ReDoS-safe bound holds
for the lexer too, not only for the pathological contrast with `re` (below).

**Reading — modes & Layout Awareness.** Contextual lexing is throughput-neutral by
construction (the dispatch runs per mode). `make bench-lex` also contrasts the modal
`python` grammar with a mono-mode baseline — the same rules with the f-string modes stripped
— on the same sample: **modal 10.62 vs mono-mode 11.02 MB/s** (~4%), and the modal path does
materially more work (53 960 vs 44 872 tokens — full f-string structure, not an opaque
string). The mode stack is per-scan; Layout Awareness reads each token's mode but adds
nothing when no mode is insignificant (an empty policy is byte-for-byte the positional pass).

## DFA fast path (opt-in) — and which modes actually accelerate

A DFA-able mode (mono-mode, greedy, assertion- and lazy-free, no code-point predicate) opts
into a `real::dfa`: one automaton pass replaces the per-rule scan. Whether a mode qualifies is
a **measured fact, not a property of the grammar's segment** — a Unicode code-point predicate
or a lazy/assertion rule makes the DFA reject the mode, which then stays on Pike. On the full
token path (`tokenize`), DFA versus the Pike row above, and the observed default-mode outcome:

| grammar | Pike MB/s | DFA MB/s | speed-up | default mode |
| --- | ---: | ---: | ---: | --- |
| json | 11.00 | 146.68 | **13.3×** | accelerates |
| sql  | 11.15 | 156.54 | **14.0×** | accelerates |
| css  |  9.00 | 133.39 | **14.8×** | accelerates |
| math |  8.00 |  81.99 | **10.2×** | accelerates |
| xml  | 13.27 | 340.29 | **25.7×** | accelerates |
| lisp |  9.05 |  9.12  | 1.0×      | stays on Pike (rejected) |
| yaml | 12.67 | 12.74  | 1.0×      | stays on Pike (rejected) |
| python\* | 10.45 | 10.61 | 1.0×  | stays on Pike (rejected) |

The split is **not** the ASCII/Unicode segment boundary: `xml` keeps Unicode shorthands yet
its default mode is DFA-representable (**25.7×**, the largest), while `lisp` is ASCII-pinned
yet its default mode carries a construct the DFA cannot represent and stays on Pike. `yaml`
and `python` (a lazy default rule) likewise stay on Pike — a transparent fallback: the token
stream is identical, only the fast path is skipped. `python\*` is the **0-regression control**
(the per-mode DFA check is free when the mode is rejected). The DFA is built once in the
constructor (≈0.3–4.2 ms one-time — a vigilance point only on very short inputs). Reproduce
with `make bench-lex`.

## Failure-cost — the recover-and-resync loop on adversarial input

When a lexer meets bytes no rule matches — a binary blob, an invalid-UTF-8 run, an unclosed
string, parasitic punctuation — a recovering lexer must skip the offending byte and resume.
This section baselines the cost of that loop **per rejected position**, on both engine paths
(an ASCII grammar whose default mode the DFA accelerates, and a Unicode text-mode grammar on
Pike). The loop is simulated over the public `tokenize` API (recover, step one byte, re-lex),
on a deterministic adversarial corpus versioned in the harness. Two costs are separated: the
raw per-position cost, and the exception-throw cost isolated on its own (an in-lexer recovery
would not throw per byte), leaving a **net per-position** figure.

| path | corpus | rejected positions | raw ns/pos | net ns/pos |
| --- | --- | ---: | ---: | ---: |
| DFA-accel (json) | binary blob        | 29 952 | 5 964 | 3 738 |
| DFA-accel (json) | invalid-UTF-8      | 32 768 | 5 957 | 3 732 |
| DFA-accel (json) | unclosed quote     | 32 768 | 5 958 | 3 732 |
| DFA-accel (json) | parasitic delims   | 32 768 | 5 971 | 3 745 |
| Pike (xml)       | binary blob        | 16 512 | 6 112 | 3 886 |
| Pike (xml)       | invalid-UTF-8      | 32 768 | 5 950 | 3 724 |
| Pike (xml)       | unclosed quote     | 0 (tolerated) | — | — |
| Pike (xml)       | parasitic delims   |  4 096 | 6 333 | 4 107 |

**Reading — three findings that shape a recovering lexer.**

1. **The exception throw dominates.** A `throw`+`catch` pair alone measures **~2 230 ns**, so
   throwing once per rejected byte is by itself larger than everything else combined. A
   recovering lexer must report the skip *without* throwing per byte.
2. **Re-lexing from scratch is setup-bound, not scan-bound.** After subtracting the throw, the
   net ~3.7 µs/position is dominated by `tokenize`'s fixed per-call setup, not the byte scan —
   which is why the DFA and Pike paths measure nearly the same here (the engine barely matters
   when a fresh `tokenize` runs at every recovery point). A recovery that reuses one cursor
   would avoid this; these figures are an upper bound.
3. **A non-fail-fast rule is O(remaining) per position.** A rule like `[^!]*!` (a maximal run
   before a terminator) that never completes scans to the end of the input before failing, so
   every recovery position re-scans what's left — **~199 000 ns/position** on an 8 KiB
   no-terminator run, and quadratic in the input. This is the characteristic a first-byte
   prefilter (`may_start_with`) mitigates by skipping positions that cannot begin the rule.

## Conditions of this baseline

| | |
| --- | --- |
| Machine | Apple Silicon (`arm64`), Darwin 23.6.0 |
| Binding | abi3 CPython extension as built by `setup.py` (`Py_LIMITED_API` 3.10) |
| Method | best-of-5 timed runs, **minimum** reported |
| As of | 2026-07-02 — C++ engine tables (per-grammar bimodal, DFA opt-in, and the new failure-cost baseline) re-measured against REAL 2026.7.5. The binding baseline below reflects the zero-copy source path + GIL release (str/bytes); the rule-count-scaling study predates the exact first-byte dispatch (noted there) |

## Binding baseline (versus `re`)

### Benign tokenization (the everyday case — `re` wins)

Tokenizing ~10 KB of ordinary `ident = ident + number * ident - number ;` soup into
4000 tokens (numbers, identifiers, operators; whitespace skipped). SciLex compiles the
rule set once (a reused `Lexer`); the `re` baseline is the standard "master pattern"
tokenizer (`(?P<NUM>…)|(?P<ID>…)|…` + `finditer`).

| tokenizer | time | vs `re` |
| --- | ---: | ---: |
| `scilex.Lexer.tokenize` | ~3.9 ms | ~2.0× |
| `re.finditer` (master pattern) | ~1.9 ms | 1.0× (baseline) |

**Reading.** `re` is ~2× faster here — down from ~5× before the binding's zero-copy
source path (it no longer re-encodes and copies the text) and the exact first-byte
dispatch (see the C++ engine table). The remaining gap buys SciLex's linear guarantee
and its ordered maximal-munch semantics, not a speed record on benign input. For
multi-threaded throughput, `tokenize` releases the GIL around the scan of inputs ≥ 4 KB;
the lazy `scan` holds the GIL per one-token step (the parser-friendly path, not the
throughput path).

### Pathological input (the linearity guarantee — SciLex wins decisively)

The classic ReDoS trigger `(a+)+b` over a run of `n` `a`s with no terminating `b`. A
backtracking engine explores `O(2ⁿ)` partitions; REAL (and therefore SciLex) is linear.

| n | `scilex` (linear) | `re.match` (backtracking) |
| ---: | ---: | ---: |
| 16 | ~2.1 µs | ~2.2 ms |
| 18 | ~2.2 µs | ~9.1 ms |
| 20 | ~2.4 µs | ~35.9 ms |
| 22 | ~2.6 µs | ~142.7 ms |
| 24 | ~2.8 µs | ~574 ms |
| 26 | ~2.9 µs | ~2.30 s |
| 1000 | ~78 µs | would not finish |

**Reading.** `re`'s time roughly **quadruples every +2** in `n` (exponential); SciLex
grows **linearly** and is still ~78 µs at `n = 1000`, where `re` would not finish in any
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

## Methodology & reproduction

- **Goal:** a regression tripwire plus an honest win/lose map — not a throughput
  contest. Compare a fresh `make bench` to this table **on the same machine**; a clear,
  repeatable change is the signal.
- **Reproduce:** `make bench-lex` compiles and runs the C++ per-grammar table (no
  Python needed); `make bench` runs that and then builds the extension in place and runs
  `benchmarks/bench.py`. The pathological sweep stops `re` once a single match passes one
  second (its curve is already established); SciLex is measured well past that.
- **Not gated.** `make bench` is excluded from `full-local-gate` on purpose — a noisy
  wall-time measurement must never turn a clean build red.
- **Grows in:** a compile-time `static_lexer` (REAL's `static_regex`) and a faster
  per-position scan (e.g. first-byte / trie dispatch) are known levers, grown in when a
  measured workload justifies them. No phantom numbers here for paths not yet built.
