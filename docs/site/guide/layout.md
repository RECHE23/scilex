# Layout

`layout` rewrites an end-of-input-terminated token stream with NEWLINE, INDENT and DEDENT inserted,
from the column of each line's first token — the offside rule of Python and YAML. The lexer's rules
skip the whitespace and newlines themselves; layout reads positions.

```pycon
>>> import scilex
>>> lx = scilex.Lexer([(0, r"[ \t]+", True), (1, r"\n", True), (2, r"[a-z]+"), (3, r"[()]")])
>>> names = {scilex.NEWLINE: "NL", scilex.INDENT: "IN", scilex.DEDENT: "DE", scilex.END_OF_INPUT: "EOF"}
>>> def show(tokens):
...     return [names.get(t.kind, t.lexeme) for t in tokens]
>>> show(lx.layout(lx.tokenize("a\n  b\n  c\nd\n", eof=True)))
['a', 'NL', 'IN', 'b', 'NL', 'c', 'NL', 'DE', 'd', 'NL', 'EOF']
```

A dedent to a column no open block used is a `scilex.LayoutError`.

## Insignificant modes

Inside brackets, a line break continues the current line. Declaring the mode of bracketed tokens
**insignificant** makes layout pass its tokens through without shaping indentation:

```pycon
>>> brackets = scilex.Lexer([
...     (0, r"[ \t\n]+", True, ["default", "paren"]),
...     (2, r"[a-z]+", False, ["default", "paren"]),
...     (3, r"\(", False, ["default", "paren"], ("push", "paren")),
...     (4, r"\)", False, ["paren"], ("pop",)),
... ], insignificant_modes=["paren"])
>>> show(brackets.layout(brackets.tokenize("f(a\n    b)\ng\n", eof=True)))
['f', '(', 'a', 'b', ')', 'NL', 'g', 'NL', 'EOF']
```

## Tabs

By default a tab counts as one column (`tabs="columns"`). `tabs="python"` measures indentation as
CPython does — tab stops of 8, cross-checked against tabs counted as 1 — and refuses a line whose level
the two readings order differently, with CPython's `TabError` message. It needs the source:

```pycon
>>> src = "a\n\tb\n"
>>> show(lx.layout(lx.tokenize(src, eof=True), source=src, tabs="python"))
['a', 'NL', 'IN', 'b', 'NL', 'DE', 'EOF']
```

In C++: `scilex::layout(tokens, mode_significant)` and
`scilex::layout(tokens, source, scilex::tab_policy::python, mode_significant)`, from the opt-in header
`scilex/layout.hpp`.
