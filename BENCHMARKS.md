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

## Measurement stamp

Every table below was measured on **Apple M1 Pro** (arm64, 8 cores), **Apple LLVM 16.0.0**
(`clang-1600.0.26.6`), `-O2`, against **real-regex 2026.7.25** (`16ff722`), on **2026-07-07**. Each case
is the **best of 9** timed passes per run, reported as the **median of N = 6 runs** (IQR quoted where it
matters). The durable content is the **ratios** (DFA-over-Pike speed-up, machine-invariant); absolute MB/s
track the host and are meaningful only under this stamp — never a bare number. Reproduce with `make bench-lex`.

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
  path both regimes now sit in the **same ~6–9.5 MB/s band** — the floor is gone.

| ASCII-pinned grammar | rules | tokens | eager MB/s | lazy MB/s |
| --- | ---: | ---: | ---: | ---: |
| json | 12 | 58 793  | 8.77 | 9.02 |
| cpp  | 41 | 52 228  | 8.39 | 8.70 |
| sql  | 39 | 38 760  | 8.04 | 8.13 |
| css  | 17 | 64 224  | 6.64 | 6.86 |
| lisp |  8 | 96 600  | 7.77 | 8.16 |
| math | 12 | 123 376 | 5.76 | 6.01 |

| Unicode text-mode grammar | rules | tokens | eager MB/s | lazy MB/s |
| --- | ---: | ---: | ---: | ---: |
| xml    | 12 | 65 588 | 9.44 | 9.84 |
| yaml   | 14 | 56 829 | 7.14 | 7.29 |
| python | 65 | 53 960 | 7.39 | 7.62 |

Three grammars are **modal** (contextual lexing): `python` (f-strings — five modes), `xml`
(content ↔ tag), `yaml` (block ↔ flow). They sit in the same band as the flat grammars —
the dispatch runs *per mode*, so modes cost throughput nothing structural. `python` carries
**65** rules (35 keywords + the modal machinery) yet holds ~7.4 MB/s: the first-byte
dispatch keeps it rule-count-independent.

Method: lexer built once, warmup then **min of 9** timed passes per run, reported as the **median of
N = 6 runs** (the stamp above), `-O2`, every result consumed through a volatile sink. Sizes are KiB
(1024 B), throughput is MB/s (10⁶ B/s). Reproduce with `make bench-lex`.

**Reading — what sets the pace.** Dispatch is **exact**: the lexer builds a 256-bucket
first-byte index from REAL's first-byte API (`has_first_byte_set` / `unique_first_byte` /
`may_start_with`), so a rule is tried at a position **only if its pattern can begin there**.
That collapses the per-position rule count, leaving throughput governed mainly by **token
density** — the cost is paid per token (one maximal-munch decision each): `math` is densest
(123 k tokens) and slowest (5.76), while the lower-density grammars top the tables (~8–9.5).

**Reading — eager vs lazy.** `scan()` edges out `tokenize()` (it never materializes the
token vector), but only just: both pay REAL's per-position NFA scan, which dominates. The
lazy path's real win is **O(1) memory**, not speed — which is why a parser prefers it.

**Reading — linearity (the guarantee, in C++).** The same `cpp` grammar over growing
inputs:

| KiB | eager MB/s |
| ---: | ---: |
| 64  | 8.54 |
| 128 | 8.41 |
| 256 | 8.39 |
| 512 | 8.38 |

Flat MB/s means time scales **linearly** with input — the linear, ReDoS-safe bound holds
for the lexer too, not only for the pathological contrast with `re` (below).

**Reading — modes & Layout Awareness.** Contextual lexing is throughput-neutral by
construction (the dispatch runs per mode). `make bench-lex` also contrasts the modal
`python` grammar with a mono-mode baseline — the same rules with the f-string modes stripped
— on the same sample: **modal 7.46 vs mono-mode 7.89 MB/s** (~5%), and the modal path does
materially more work (53 960 vs 44 872 tokens — full f-string structure, not an opaque
string). The mode stack is per-scan; Layout Awareness reads each token's mode but adds
nothing when no mode is insignificant (an empty policy is byte-for-byte the positional pass).

## DFA fast path (opt-in) — every example grammar accelerates, 3–27×

A DFA-able mode (mono-mode, greedy, assertion- and lazy-free, no code-point predicate) opts into a
`real::dfa`: one automaton pass replaces the per-rule scan. Whether a mode qualifies is a **measured
fact** — a rule the DFA cannot represent makes it reject the mode, which then stays on Pike (the
fallback below). On the full token path (`tokenize`), DFA versus the Pike baseline:

| grammar | Pike MB/s | DFA MB/s | speed-up |
| --- | ---: | ---: | ---: |
| xml      | 9.45 | 251.48 | **26.7×** |
| css      | 6.71 | 142.52 | **21.4×** |
| sql      | 8.00 | 157.80 | **19.6×** |
| json     | 8.75 | 149.18 | **17.0×** |
| math     | 5.75 |  87.34 | **15.2×** |
| lisp     | 7.73 |  98.64 | **12.8×** |
| yaml     | 7.22 |  35.53 |  **4.9×** |
| python\* | 7.48 |  23.01 |  **3.1×** |

**Every one of the example grammars now accelerates — 3.1× to 26.7×** (the dense ASCII grammars and
`xml` gain the most, ~15–27×; sparse or lightly-tokenized modes like `yaml` and the `python*` control
gain least, ~3–5×; full-set geomean ~13×, dense-set ~20×). This is **new in this release**: real-regex
2026.7.25's DFA, together with the SCILEX-1 example fixes, made `lisp`, `yaml` and `python`'s default
modes DFA-representable where they previously stayed on Pike.

The **transparent fallback** is unchanged — a mode the DFA cannot represent (a non-head assertion, a lazy
quantifier) is caught at build time (`real::dfa_error` or the longest-vs-shortest audit) and lexes on Pike
with a byte-identical token stream. Since 2026.7.25 no shipped example grammar needs it, so it lives as a
**deterministic test** rather than a benchmark row: `dfa_modes_fallback_on_assertion` forces a `$` assertion
into a mode, asserts the eligibility report drops it (`dfa_modes_active()` empty), and checks the tokens
still match a brute-force maximal munch. The DFA is built once in the constructor (≈0.4–4.3 ms one-time — a
vigilance point only on very short inputs). Reproduce with `make bench-lex`.

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

## Cross-tool comparison — other lexers on the same input

The C++ tables above are SciLex's own engine. This section places the **Python-embedded** lexer
(`scilex` the extension) beside other tokenizers on the *same* files and the *same* task — a full
tokenization pass — timed on the shared `sciforge.bench` substrate (warmed, best-of-N, 95% bootstrap
CI). The numbers are **not cherry-picked**: where a tool beats SciLex, its number is here as measured.

Corpus: a ~515 KB JSON document and a ~512 KB block of ordinary Python source, both generated
deterministically by the harness (`benchmarks/bench_compare.py`, cached under `benchmarks/data/`).

| Tool | `big.json` (MB/s) | `sample.py` (MB/s) | What it produces |
| --- | --- | --- | --- |
| **scilex** `tokenize()` | 5.4 | 6.9 | a token stream as Python objects |
| **scilex** `scan()` (lazy) | 5.3 | 6.6 | the same, lazily (fully consumed here) |
| Pygments | 10.4 | 0.8 | pure-Python styled (type, text) pairs — a highlighting superset |
| tree-sitter | 12.9 | 10.6 | a full parse tree in C, returned as a handle |
| flex (codegen) | 162.6 | — | a compile-time DFA scanner (C), best-of-30 internal |

**Read this with the comparability notes — the tools do different amounts of work:**

- **SciLex's figure includes materialising a Python object per token** (the binding cost). The C++
  engine itself, without the binding, runs far faster — that is the per-grammar table at the top of
  this page, not these rows. As an *embedded Python lexer producing Python tokens*, SciLex is mid-pack.
- **tree-sitter** builds a full parse tree in C and returns a handle; no per-token Python object is
  created (walking the tree would add that), which is much of why its number is high. It is also
  **incremental** — a capability this one-shot pass does not exercise, and a genuine tree-sitter win.
- **Pygments** is pure Python and produces styled pairs for highlighting; it leads on JSON and trails
  badly on Python here (its Python lexer does substantially more per token).
- **flex** is the raw-throughput **ceiling**: a code-generated native DFA with a build step and a fixed
  grammar — exactly the axis SciLex does *not* compete on (grammar-as-data, linear-safe, modes/layout).
  It is ~15–30× any Python-embedded option, which is the honest shape of that trade.
- **Not measured:** Logos and re2c (Rust / a separate C-codegen toolchain) — named, not benchmarked.

**The honest reading.** On raw embedded-Python throughput SciLex is neither the fastest nor the
slowest: tree-sitter's C-tree return beats it, Pygments beats it on JSON and loses on Python, and a
code generator (flex) beats everyone. SciLex's case is not this number — it is a linear-time,
ReDoS-safe lexer whose grammar is runtime data, with modes, layout, and recovery, callable from C++
and Python. The comparison confirms the positioning in the [axes page](@ref comparison), it does not
overturn it.

## Methodology & reproduction

- **Goal:** a regression tripwire plus an honest win/lose map — not a throughput
  contest. Compare a fresh `make bench` to this table **on the same machine**; a clear,
  repeatable change is the signal.
- **Cross-tool:** `python3 benchmarks/bench_compare.py` (optionally `--json`). Its competitor
  dependencies are **optional** — Pygments, `tree_sitter` + the grammar packs, and `flex` + a C
  compiler are each skipped with a note if absent, never a hard failure. Figures above were taken with
  all present.
- **Reproduce:** `make bench-lex` compiles and runs the C++ per-grammar table (no
  Python needed); `make bench` runs that and then builds the extension in place and runs
  `benchmarks/bench.py`. The pathological sweep stops `re` once a single match passes one
  second (its curve is already established); SciLex is measured well past that.
- **Not gated.** `make bench` is excluded from `full-local-gate` on purpose — a noisy
  wall-time measurement must never turn a clean build red.
- **Grows in:** a compile-time `static_lexer` (REAL's `static_regex`) and a faster
  per-position scan (e.g. first-byte / trie dispatch) are known levers, grown in when a
  measured workload justifies them. No phantom numbers here for paths not yet built.
