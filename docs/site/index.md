# SciLex

**A maximal-munch lexer whose grammar is data, on a regex engine that cannot backtrack.**

SciLex turns an ordered list of token rules — each a [REAL](https://reche23.github.io/real-regex/)
regular expression — into a tokenizer. The longest match wins and rule order breaks ties; a rule can
live in named modes and push, pop or replace the mode it wins in; indentation can become
NEWLINE / INDENT / DEDENT tokens. Grammars are C++ rule lists, Python tuples or `.lex` files, and
every mode whose rules a DFA reproduces exactly runs on one, automatically.

::::{grid} 1 2 2 3
:gutter: 3

:::{grid-item-card} Get started
:link: getting-started
:link-type: doc

Install the header-only library, the Python package or the CLI, and lex your first input.
:::

:::{grid-item-card} Tutorial
:link: tutorial
:link-type: doc

A JSON lexer, then a string mode with escapes, then significant indentation.
:::

:::{grid-item-card} Guides
:link: guide/index
:link-type: doc

The `.lex` format, modes, layout, errors and recovery, positions, performance, threads.
:::

:::{grid-item-card} C++ reference
:link: reference/cpp
:link-type: doc

`scilex::lexer`, `rule`, `token`, `position`, `layout`, `parse_grammar`.
:::

:::{grid-item-card} Python reference
:link: reference/python
:link-type: doc

`scilex.Lexer`, `Token`, `Layout`, `parse_grammar`, the exception classes.
:::

:::{grid-item-card} Security
:link: security
:link-type: doc

What is linear, what is quadratic, and what a grammar supplier can make expensive.
:::
::::

## What it guarantees

- **No input makes the scan exponential.** Every rule is a `real::regex`, which never backtracks.
- **Linear on the DFA.** The rules a mode's DFA takes are memoized over the source, so together they
  cost O(n·Q) on every input; the worst case is quadratic, and only through a rule left on the
  per-rule path that scans far and loses at every position.
- **The same tokens either way.** The DFA is an optimizer: a mode keeps it only where the lexer has
  proved it reproduces the per-rule munch, rule by rule.

## A first look

```pycon
>>> import scilex
>>> lx = scilex.Lexer([
...     (0, r"\s+", True),               # whitespace, skipped
...     (1, r"[0-9]+"),                  # number
...     (2, r"[A-Za-z_][A-Za-z0-9_]*"),  # identifier
...     (3, r"[-+*/=]"),                 # operator
... ])
>>> [(t.kind, t.lexeme) for t in lx.tokenize("x = 41 + 1")]
[(2, 'x'), (3, '='), (1, '41'), (3, '+'), (1, '1')]
```

```{toctree}
:hidden:
:maxdepth: 2

getting-started
tutorial
guide/index
reference/index
security
```
