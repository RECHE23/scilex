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
| benign token soup | `re` (~5×, see B1) | a mature C backtracking engine; SciLex runs REAL's NFA per position, through the binding, and builds rich `Token`s |
| adversarial / ReDoS (B2) | **SciLex** (linear vs exponential) | REAL is linear-time and ReDoS-safe; `re` backtracks catastrophically |
| untrusted / machine-generated | **SciLex** | the linear bound holds on *every* input — no pathological cliff |

## C++ engine throughput — per grammar

`bench.py` (below) measures the **Python binding**; this section measures the **C++
engine directly** — the speed a C++ embedder or a SciParse parser sees, with no
interpreter in the path. `make bench-lex` lexes each of the seven example grammars
(`examples/<lang>.hpp`) over its own sample scaled to a ~256 KiB steady-state input,
reporting MB/s for `tokenize()` (eager, full token vector) and `scan()` (lazy, O(1)
memory — the parser path).

| grammar | rules | tokens | eager MB/s | lazy MB/s |
| --- | ---: | ---: | ---: | ---: |
| json   | 12 | 58 793  | 2.48 | 2.49 |
| lisp   |  8 | 96 600  | 1.63 | 1.66 |
| cpp    | 41 | 52 228  | 1.80 | 1.83 |
| css    | 17 | 64 224  | 1.18 | 1.18 |
| math   | 12 | 123 376 | 1.17 | 1.18 |
| python | 36 | 89 613  | 1.11 | 1.12 |
| sql    | 39 | 38 760  | 0.80 | 0.80 |

Method: lexer built once, warmup then **min of 9** timed passes, `-O2`, every result
consumed through a volatile sink. Sizes are KiB (1024 B), throughput is MB/s (10⁶ B/s),
same machine as the conditions below. Reproduce with `make bench-lex`.

**Reading — what sets the pace.** Two levers, both visible in the table:

1. *Token density.* The cost is paid **per token** — each token is one maximal-munch
   decision. `json` and `math` share a 12-rule set, yet `math` runs ~2× slower because
   its sample packs ~2× the tokens per KB (123 k vs 59 k): throughput tracks KB ÷ tokens.
2. *Rules in the general list.* A rule whose pattern has no single fixed leading byte is
   tried at **every** position. `sql` has the **fewest** tokens of the seven yet is the
   **slowest** — its 31 case-insensitive keyword rules carry the `icase` flag, and the
   first-byte dispatch deliberately refuses to bucket a flagged literal (the case-fold
   could match another byte), so all 31 land in the general list and are attempted byte
   by byte. `cpp` has an equal keyword count but as **plain** literals (first-byte
   bucketed), and runs 2.3× faster at a comparable total rule count.

This is the measured motivation for the next perf step (couple the dispatch to REAL's
`pattern_hints`): REAL already knows an `icase` literal's possible leading bytes, so such
rules could rejoin the dispatch instead of the general list — directly lifting the `sql`
floor.

**Reading — eager vs lazy.** `scan()` edges out `tokenize()` (it never materializes the
token vector), but only just: both pay REAL's per-position NFA scan, which dominates. The
lazy path's real win is **O(1) memory**, not speed — which is why a parser prefers it.

**Reading — linearity (the guarantee, in C++).** The same `cpp` grammar over growing
inputs:

| KiB | eager MB/s |
| ---: | ---: |
| 64  | 1.82 |
| 128 | 1.80 |
| 256 | 1.81 |
| 512 | 1.81 |

Flat MB/s means time scales **linearly** with input — REAL's linear, ReDoS-safe bound
holds for the lexer too, not only for the pathological `re` contrast in B2 below.

**The honest position.** 0.8–2.5 MB/s is **slower** than `re` or `flex` (tens to hundreds
of MB/s) on benign input — the price of running a real NFA at every position and building
ordered maximal-munch `Token`s, in exchange for the linear guarantee. The dispatch/hints
step above is the identified lever to narrow that gap where it is widest (flag- and
class-led rules).

## Conditions of this baseline

| | |
| --- | --- |
| Machine | Apple Silicon (`arm64`), Darwin 23.6.0 |
| Binding | abi3 CPython extension as built by `setup.py` (`Py_LIMITED_API` 3.10) |
| Method | best-of-5 timed runs, **minimum** reported |
| As of | 2026-06-20 — added the per-grammar C++ engine table (`make bench-lex`); binding rows from v2026.6.1 (scan/eof/layout, dispatch, `.context`) |

## Binding baseline (versus `re`)

### B1. Benign tokenization (the everyday case — `re` wins)

Tokenizing ~10 KB of ordinary `ident = ident + number * ident - number ;` soup into
4000 tokens (numbers, identifiers, operators; whitespace skipped). SciLex compiles the
rule set once (a reused `Lexer`); the `re` baseline is the standard "master pattern"
tokenizer (`(?P<NUM>…)|(?P<ID>…)|…` + `finditer`).

| tokenizer | time | vs `re` |
| --- | ---: | ---: |
| `scilex.Lexer.tokenize` | ~7.4 ms | ~5.2× |
| `re.finditer` (master pattern) | ~1.4 ms | 1.0× (baseline) |

**Reading.** `re` is ~5× faster here. That is expected and reported plainly: the cost
buys SciLex's linear guarantee and its ordered maximal-munch semantics, not a speed
record on benign input. Tightening the per-position scan is a known lever, pursued
when a real workload makes it the bottleneck.

### B2. Pathological input (the linearity guarantee — SciLex wins decisively)

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

### B3. Rule-count scaling — the first-byte dispatch

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

**Verdict.** Implemented (data-backed, measured ~5× on a realistic 46-rule lexer).
Aho-Corasick / a fuller prefilter are not warranted yet (no data demands them); bucketing
*class* leads (not just literals) is a possible further step if a future workload shows the
remaining ~6 ms floor matters.

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
