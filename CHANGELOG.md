# Changelog

All notable changes to SciLex. Versions are calendar-based (`YYYY.M.PATCH`, the
patch resetting each month; PEP 440 drops leading zeros). The project holds the
SciLang-stack gate: 100%-4D coverage of `include/`, dual-compiler, sanitizers, and a
fuzz oracle.

## 2026.6.5

The contextual-lexing release: **modes** and **Layout Awareness**. Ambitious but
honest — here is what it does, and what is deliberately left for Level B.

### Added
- **Modes (contextual lexing).** A rule may carry `in_mode` (the modes it is active
  in) and an `action` (push / pop / set the per-scan mode stack), so the same byte
  lexes differently by context. Maximal munch and the exact first-byte dispatch run
  *per mode*; the lexer stays immutable and shareable (the stack is per-scan).
- **Layout Awareness (Level A).** Each token carries the mode it was lexed in; a mode
  can be marked *insignificant*, and the layout pass then passes its tokens through
  without shaping indentation.
- **Three modal example grammars** — Python f-strings (five modes), XML
  (content ↔ tag), YAML (block ↔ flow) — nine examples in all, each fuzzed by the
  oracle against an independent brute-force reference.

### Lifted (by Layout Awareness Level A)
- YAML multi-line flow collections no longer pick up spurious INDENT/DEDENT.
- Python implicit line continuation inside `()` `[]` `{}` is no longer read as a new
  block.

### Deferred (honest scope)
- **Block scalars** (`|` / `>`) and heredocs need a reference indent carried in the
  mode frame — that is **Layout Awareness Level B**, designed but not built.
- A compile-time `static_lexer` (on REAL's `static_regex`) — not yet warranted.

### API
- `scilex::rule` gains `in_mode` (a set of mode names) and `action` (a `mode_action`:
  push / pop / set); a plain `{kind, pattern, skip}` rule is unchanged.
- `scilex::lexer(rules, insignificant_modes = {})`; accessors `mode_significant()`
  and `mode_name(id)`; `scilex::layout(tokens, mode_significant = {})` (mode-aware).
- `scilex::token` gains `mode_id` (default 0; existing aggregate inits compile
  unchanged).
- Python: `Lexer(rules, insignificant_modes=())`, `Lexer.layout(tokens)`,
  `Token.mode` (part of `==` / `hash`), `Layout(insignificant_modes)` and
  `layout(tokens, insignificant_modes)`. The low-level token tuple is now 6-field
  (`kind, lexeme, offset, line, column, mode`).
- Two invariants: an empty significance policy is byte-for-byte the positional pass
  (zero cost); the **mode** is the single source of the policy — no per-rule flag.

## 2026.6.4

Baseline before modes: the exact first-byte dispatch (REAL's first-byte API, 3–7×
over the prior textual heuristic), the zero-copy / GIL-releasing Python binding
(str + bytes, `py.typed`), the `scilex` CLI, the fuzz oracle, and seven example
grammars. See the git history for earlier calendar versions.
