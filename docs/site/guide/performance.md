# Performance

## Two engines, one token stream

Every rule is a `real::regex`. The **per-rule path** tries, at each position, the rules whose pattern
can begin with the byte there (an exact first-byte index), and keeps the longest match — the lowest
index on a tie. The **DFA path** runs one `real::dfa` over a mode's rules at once: one table step per
byte, whatever the rule count.

A lexer tries the DFA in every mode by default (`dfa="auto"`, C++ `dfa_policy::automatic`). A rule joins
its mode's DFA when the lexer proves that the DFA reproduces it: its `match()` must be its longest match,
and it must need no assertion a DFA cannot represent. The other rules — a `\b`, a `$`, a lookaround, a
Unicode `\w`, a lazy delimiter — stay on the per-rule path beside the DFA, and the munch merges the two
by the same rule. The tokens are identical either way.

```pycon
>>> import scilex
>>> lx = scilex.Lexer([(0, r"\s+", True), (1, r"[a-z]+"), (2, r"\b[0-9]+\b")])
>>> lx.dfa_modes_active
['default']
>>> lx.pike_rules()     # the rule with \b stays on the per-rule path
[2]
```

`dfa="requested"` (C++ `dfa_policy::requested`) limits the DFA to the modes named in `dfa_modes`; an
empty list keeps everything on the per-rule path.

## Complexity

- The rules on a DFA cost **O(n·Q)** together on every input, `Q` the DFA's states: each walk is
  memoized over the source, so a state a walk proved leads to no accept stops every later walk that
  reaches it.
- The rules on the per-rule path cost **O(n·S·m)**, `S` the longest scan a rule makes before its
  threads die and `m` its program size. Grammars whose rules stop scanning near their tokens are linear;
  a rule that scans far and loses at every position is quadratic.
- No input is exponential: REAL never backtracks.

The memo costs nothing until a walk runs more than 32 bytes past its last accept; from then on it holds
up to one bit per source byte for each DFA state such a stretch passed, at most Q·n bits per mode. A
`scan()` over an input that never arms it holds only the source and the mode stack.

## Measured

On the example grammars, the default lexer runs at 66–134 MB/s against 7–14 MB/s on the per-rule path
alone — 5.7× to 18× on arm64 and 8.6× to 27.5× on x86-64, where the per-rule path reads lower. Through
the Python binding SciLex is 1.3–1.9× faster than a `re`-based tokenizer on the benign case measured,
and ahead of Pygments and tree-sitter on the cross-tool corpora. The tables, their stamps and their
method are in
[BENCHMARKS.md](https://github.com/RECHE23/scilex/blob/main/BENCHMARKS.md).

## Choosing patterns

- Pin `(?a)` (or `real::flags::ascii`) where identifiers are ASCII by specification: an ASCII `\w` stays
  on the DFA, a Unicode `\w` does not.
- Prefer a greedy delimiter written so that its `match()` is its longest match — `"(?:[^"\\]|\\.)*"`
  rather than `".*?"` — which keeps the rule on the DFA.
- Give recovery-heavy rules a definite leading byte, so error runs are skipped a byte test at a time.
