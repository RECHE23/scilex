# SciLex — performance baseline

A reproducible wall-time baseline for the Python binding, comparing SciLex against
Python's standard-library `re`. Its purpose is twofold: a **regression tripwire**
between versions on the same machine, and an honest statement of **where SciLex wins
and where it does not**.

Run it with **`make bench`** (or `python benchmarks/bench.py`). It is informational
only — it prints a table, is never invoked by `full-local-gate`, and never fails a
build.

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

## Conditions of this baseline

| | |
| --- | --- |
| Machine | Apple Silicon (`arm64`), Darwin 23.6.0 |
| Binding | abi3 CPython extension as built by `setup.py` (`Py_LIMITED_API` 3.10) |
| Method | best-of-5 timed runs, **minimum** reported |
| As of | 2026-06-19 — the Python-binding maturation reserve |

## Baseline

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
record on benign input. Tightening the per-position scan is a known, deliberately
deferred lever — to be pursued only if a real workload makes it the bottleneck.

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

## Methodology & reproduction

- **Goal:** a regression tripwire plus an honest win/lose map — not a throughput
  contest. Compare a fresh `make bench` to this table **on the same machine**; a clear,
  repeatable change is the signal.
- **Reproduce:** `make bench` builds the extension in place and runs
  `benchmarks/bench.py`. The pathological sweep stops `re` once a single match passes
  one second (its curve is already established); SciLex is measured well past that.
- **Not gated.** `make bench` is excluded from `full-local-gate` on purpose — a noisy
  wall-time measurement must never turn a clean build red.
- **Deferred:** a compile-time `static_lexer` (REAL's `static_regex`) and a faster
  per-position scan (e.g. first-byte / trie dispatch) are known levers, left until a
  measured workload justifies them. No phantom numbers here for paths not yet built.
