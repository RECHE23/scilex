# Releasing SciLex

`make release` (SciForge's `make/release.mk`) computes the next calendar version `YYYY.M.PATCH` — the patch resets
each month, the first release of a month is `.0`, PEP 440 drops leading zeros — writes it into `pyproject.toml`,
`python/scilex/__init__.py` and `include/scilex/version.hpp`, files `CHANGELOG.md`'s `## Unreleased` section under
it, then commits, tags and pushes from a clean `main` whose CI is green. The tag drives `release.yml`: the abi3
wheels and the sdist, published to PyPI by Trusted Publishing, and the GitHub Release.

## Versions and breaking changes

A change that can break a user's code or the tokens it depends on — a removed or renamed name, a changed signature
or default, an enumerator's value, a different token stream for the same grammar and input, a `.lex` file read
otherwise — is listed under a **Breaking** heading of its release's CHANGELOG entry and held for the first release
of a month (`YYYY.M.0`), so that the patch releases within a month only add and fix. Raising the floor of REAL
(`real-regex>=`) is not breaking by itself; it is noted under **Changed** with what the newer REAL fixes.

## Release cadence

A change goes out when it is ready: a correctness fix, an addition or a speed-up is released as soon as the release
checks pass (`make check-release`), carrying `main` as it is. Only a change that can break existing code waits: it is
announced under **Breaking** in the CHANGELOG's `## Unreleased` section and released with the first release of a month
(`YYYY.M.0`), so that the releases within a month only add, speed up and fix.

## Stable surface

What the policy above protects, and what it does not yet:

- **Stable — C++.** In namespace `scilex`: `rule` (`kind`, `pattern`, `skip`, `in_mode`, `action`) and
  `mode_action`; `lexer` with `tokenize`, `scan`, `end_of`, `dfa_modes_active` and `pike_rules`; `token`,
  `position`; `eof_policy`, `error_policy`, `column_unit`, `dfa_policy`; `lex_error`; the indentation pass
  `layout` (Level A) with `tab_policy` and `layout_error`; the reserved kinds (`end_of_input`, `newline`, `indent`,
  `dedent`, `error`). Include `<scilex/scilex.hpp>`.
- **Stable — Python.** `Lexer` (`tokenize`, `scan`), `Token`, `Position`, `Layout`, `tokenize`, `scan`, `layout`,
  the errors (`LexerError`, `LexError`, `LayoutError`) and the reserved kinds.
- **Experimental**, landed within the last weeks and still moving; each may change in a patch release, noted under
  **Changed**:
  - text fed in pieces: `token_stream`, Python `Lexer.stream()` and `TokenStream`;
  - the `.lex` grammar format and its reader: `grammar.hpp`, `Grammar`, `parse_grammar`, `load_grammar`,
    `GrammarError`, and the `scilex` CLI's options and output;
  - Layout Level B, the indentation steered by rules: `rule::indent`, `rule::extend`, `rule::continuation`,
    `indent_role`, `extension`;
  - the context predicates: `rule::after`, `rule::not_after`, `rule::at_line_start`, and the insignificant modes.
- **Tuning, not a contract.** `get_config`, `real_version`: they describe this build.
- **Internal.** Namespace `scilex::detail`, every `SCILEX_*` macro, the headers' layout (include
  `<scilex/scilex.hpp>`, not a file within it), and which rules run on the DFA or on Pike: the token stream is the
  same either way.

## Correctness record

The day of the last correctness finding — a wrong token stream, a crash, a hang, or a broken guarantee — in a
released version, and the release that fixed it. The library is called mature once six weeks have passed without
one, six weekly long fuzzing campaigns green (`.github/workflows/fuzz-long.yml`) beside the oracles of every push.

| Last finding | What | Fixed in |
| --- | --- | --- |
| 2026-10-14 | wrong tokens in the YAML grammar and its layout (document markers past a line's start, a flow scalar over a line break, an indented first token, an anchor ended by `:`), a token open at a piece's end read twice in a stream | 2026.10.10 |
