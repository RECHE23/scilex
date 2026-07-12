# Changelog

All notable changes to SciLex. Versions are calendar-based (`YYYY.M.PATCH`, the
patch resetting each month; PEP 440 drops leading zeros). The project holds the
SciLang-stack gate: 100%-4D coverage of `include/`, dual-compiler, sanitizers, and a
fuzz oracle.

## Unreleased

## 2026.7.4 — 2026-07-12

### Fixed
- **`flags::ascii` grammars tokenized four control separators (FS/GS/RS/US, `U+001C`-`U+001F`)
  backwards.** real-regex 2026.7.37 fixed ASCII-mode `\s` to exclude them (matching Python `re`'s own
  `re.ASCII` contract; only text-mode `\s` includes them). Any SciLex grammar with a `\s`/`\S` rule
  pinned to `real::flags::ascii` — every DFA-compat grammar, via `plain()`'s own convention — now
  classifies these four bytes the same way `re` does. Regression pinned by pin-flip: fails cleanly
  (non-crashing) against real-regex < 2026.7.37, passes against 2026.7.37+.

### Changed
- **Build requires `real-regex >= 2026.7.37`** (was `>= 2026.7.25`), for the fix above.

## 2026.7.3

### Added
- **Every example grammar now DFA-accelerates (3–27×).** Bumping to real-regex 2026.7.25 (below) makes
  `lisp`, `yaml` and `python`'s default modes DFA-representable — previously they fell back to Pike. With the
  SCILEX-1 example fixes, all nine example grammars now accelerate on `dfa_modes` (dense grammars ~15–27×,
  sparser ones 3–5×; full-set geomean ~13×). See BENCHMARKS.md, refreshed with a full environment stamp. The
  transparent Pike fallback is unchanged and covered by the `dfa_modes_fallback_on_assertion` test.

### Fixed
- **Lexer mode sets are a `std::vector<std::string>`, not a `std::unordered_set`.** The unordered-set
  instantiation drifted symbols across the libc++ ABI boundary (the binding is abi3); an ordered vector is
  symbol-stable, a mode set is tiny so membership cost is unchanged, and the token stream is identical.

### Changed
- **Build requires `real-regex >= 2026.7.25`** (was `>= 2026.7.5`), tracking the current REAL release —
  the linear POSIX grammars, bounded lookarounds, Unicode `\w \d \s`, and the per-operation ReDoS-safety
  guarantee the lexer builds on.

## 2026.6.6

### Added
- **DFA fast path (opt-in).** `lexer(rules, insignificant_modes={}, dfa_modes={})` plus
  `dfa_modes_active()`; the Python `Lexer(rules, insignificant_modes=(), dfa_modes=())`
  with `dfa_modes` / `dfa_modes_active` properties. A named mode is accelerated by a
  `real::dfa` — one maximal-munch pass replacing the per-rule dispatch (a large speed-up on the
  full token path for DFA-able modes; see BENCHMARKS.md for current, stamped per-grammar figures).
  Additive and invisible: the Pike engine stays the
  floor, a mode whose rules need an assertion no DFA can represent or whose DFA fails a
  build-time audit (a lazy quantifier) silently falls back to Pike, and the token stream
  is byte-identical either way. The `sql` and `css` example grammars opt in.

*(Entries for 2026.6.7 – 2026.7.2 were not kept here per-release; those git tags are the record. Per-release
entries resume above.)*

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

### Known limitations (Level B)
- **Block scalars** (`|` / `>`) and **heredocs** need a reference indent carried in
  the mode frame — that is **Layout Awareness Level B**, designed but not built.
  (Multi-line flow collections and implicit line continuation are *not* limitations
  — Layout Awareness Level A lifts both, this release.)

### Not yet
- A compile-time `static_lexer` (on REAL's `static_regex`) — grows in on demand.

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
