# Security Policy

## Supported versions

SciLex ships under CalVer (`YYYY.M.PATCH`). Only the **most recent release** receives fixes; older
releases do not.

| Version | Supported |
| --- | --- |
| latest release | ✅ |
| older releases | ❌ |

## Reporting a vulnerability

Please report security issues privately through GitHub's
**[Report a vulnerability](https://github.com/RECHE23/scilex/security/advisories/new)** (private
vulnerability reporting). Do **not** open a public issue for a security report.

## Threat model

SciLex is a generic lexer with two distinct trust boundaries. Treat them separately.

### 1. Untrusted input text (the primary boundary)

The text you tokenize is assumed untrusted — a lexer's whole job is to be fed hostile bytes. SciLex
runs each rule as a `real::regex`, which is **linear-time and ReDoS-safe**: a match attempt costs
O(s·m), `s` the bytes it scans and `m` the program size, and nothing backtracks. That closes the ReDoS
door *below* SciLex — no input makes the scan exponential.

**It is not a linear bound on every input, and this policy does not claim one.** At each token start
every candidate rule is attempted, and a rule may scan far past the token that finally wins, so
tokenizing costs O(n·S·m), `S` the longest such scan. With a fixed grammar whose rules scan far and
lose, the input decides `S`: `a*b` and `a` on `aaa…` scan to the end at every position, and the time
grows **quadratically** — measured 2026-09-23, each doubling of the input multiplies it by ~4. The
shipped example grammars stay linear on the inputs measured in `BENCHMARKS.md`. If you tokenize
untrusted text, write rules that stop scanning near their tokens, and bound the input size where the
grammar cannot guarantee that. The full statement is in `docs/spec.dox` (complexity section).

**In scope:** an input that makes any fixed grammar scan **super-quadratically** (or exponentially) in
the input length; a quadratic case in a **shipped example grammar**; any crash, hang or memory-safety
issue from input text. Report through the channel above with the grammar, the input (or a generator),
and the observed scaling.

Error recovery (`error_policy::token`) never throws per byte; malformed and binary input is emitted as
`error` tokens, not a crash or a hang. It has the same shape as the scan: a rule that scans far before
failing pays that scan at every position of an error run.

### 2. The grammar (a second, narrower boundary)

The rule set — including a `.lex` grammar loaded from a file by the CLI — is authored, not arbitrary
attacker input, but it still carries two costs a grammar author should know:

- **DFA build cost.** Every mode is tried for the DFA fast path (`dfa_policy::automatic`, the
  default), which builds a `real::dfa` at construction. Subset construction is bounded by a hard **65 536-state cap** (in `real::dfa`): a
  grammar whose mode would exceed it raises `dfa_error` rather than consuming unbounded time or memory.
  The token stream is unaffected either way (Pike is the floor).
- **Recovery cost of a non-fail-fast rule.** Under `error_policy::token`, an unanchored, greedy rule
  that scans far before failing (a `.*x`-style rule with no distinguishing first byte) pays that scan at
  *every* position of a long error run — O(remaining) per position, quadratic on hostile input. This is
  a property of the grammar, documented in the failure-cost baseline (see `BENCHMARKS.md`); prefer rules
  with a definite leading byte if recovery speed on hostile input matters. The non-recovery path has
  the same exposure (section 1): the bound is a property of the grammar's rules, not of SciLex alone.

A grammar that provokes super-linear *build* time within the DFA cap, or any memory-safety issue from a
crafted grammar, is in scope — report it.
