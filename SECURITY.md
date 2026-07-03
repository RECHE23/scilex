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
runs each rule as a `real::regex`, which is **linear-time and ReDoS-safe**, and the cursor only ever
advances, so tokenizing is **O(n·m)** for input length `n` and program size `m` — linear in the input
for *every* input, with no pathological cliff. That inherited guarantee is the point: the ReDoS door is
closed *below* SciLex, so a grammar cannot be tricked into super-linear scanning by the input.

**A bypass of the linear-time guarantee is a vulnerability.** If you find a fixed grammar and an input
for which SciLex is **not** linear in the input, report it through the channel above with the grammar,
the input (or a generator), and the observed scaling.

Error recovery (`error_policy::token`) never throws per byte and stays within the same linear bound on
input length; malformed and binary input is emitted as `error` tokens, not a crash or a hang.

### 2. The grammar (a second, narrower boundary)

The rule set — including a `.lex` grammar loaded from a file by the CLI — is authored, not arbitrary
attacker input, but it still carries two costs a grammar author should know:

- **DFA build cost.** Opting a mode into the DFA fast path (`dfa_modes`) builds a `real::dfa` at
  construction. Subset construction is bounded by a hard **65 536-state cap** (in `real::dfa`): a
  grammar whose mode would exceed it raises `dfa_error` rather than consuming unbounded time or memory.
  The token stream is unaffected either way (Pike is the floor).
- **Recovery cost of a non-fail-fast rule.** Under `error_policy::token`, an unanchored, greedy rule
  that scans far before failing (a `.*x`-style rule with no distinguishing first byte) pays that scan at
  *every* position of a long error run — O(remaining) per position, quadratic on hostile input. This is
  a property of the grammar, documented in the failure-cost baseline (see `BENCHMARKS.md`); prefer rules
  with a definite leading byte if recovery speed on hostile input matters. It does not affect the
  linear guarantee for the non-recovery path.

A grammar that provokes super-linear *build* time within the DFA cap, or any memory-safety issue from a
crafted grammar, is in scope — report it.
