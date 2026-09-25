# Security

SciLex inherits REAL's guarantee: no regular expression can make the scan backtrack, so **no input is
exponential**. What remains is a matter of constants and of one quadratic case:

- **Rules on a DFA are linear together**, O(n·Q), on every input.
- **A rule on the per-rule path that scans far and loses** at every position is quadratic: `a*b` beside
  `a` over `aaa…` rescans the rest of the input from each position. The same pair on the DFA is linear.
  `pike_rules(mode)` names the rules on that path.
- **A grammar supplier controls the constants**: the DFA's size (capped; a mode past the cap stays on the
  per-rule path), the rule count, and each pattern's cost inside REAL.
- **Memory is bounded**: the mode stack by `max_mode_depth`, the walk memo by Q·n bits per mode once
  armed.

Treat a grammar from an untrusted source as you would treat a regex from one: bound the input length,
and measure the grammar on adversarial input before accepting it. The full policy — what counts as a
vulnerability and how to report one — is
[SECURITY.md](https://github.com/RECHE23/scilex/blob/main/SECURITY.md).
