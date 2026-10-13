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

## Level B: tokens bounded by indentation

Some tokens end where the indentation of the block they belong to says: a YAML block scalar (`|`, `>`) runs
over every line indented past its block, and a plain scalar continues on such lines. No pattern knows that
indentation, so the lexer tracks it: it keeps a stack of open blocks' columns, as YAML's scanner does, updated
by each emitted token whose rule has a role — `entry` opens a block at its column (`-`, `?`), `key` marks the
line's key candidate, `value` opens a block at that candidate's column (`:`), `reset` closes them all (`---`).
A rule's `extend` then reads the innermost open block: `block` takes the body indented past it (a digit in the
match fixes the body's indentation), `lines` the lines indented past it, each read by the rule's
`continuation` pattern (by default its own):

```pycon
>>> yamlish = scilex.Lexer([
...     (0, r"[ \t\n]+", True),
...     (1, r"-", False, [], None, None, None, "entry"),
...     (2, r":", False, [], None, None, None, "value"),
...     (3, r"\|", False, [], None, None, None, None, "block"),
...     (4, r"[a-z]+( +[a-z]+)*", False, [], None, None, None, "key", "lines"),
... ])
>>> [t.lexeme for t in yamlish.tokenize("- run: |\n    make\n    make test\n  name: long\n    title\n")]
['-', 'run', ':', '|\n    make\n    make test\n', 'name', ':', 'long\n    title']
```

An insignificant mode takes no part, a stream decides such a token once its lines are complete, and layout
reads where it starts, so its body adds no structure.

The roles shape `layout` as well. The node past an entry opens a block at its column when it is an entry
itself or a value follows it on its line, so the compact `- key: v` nests its mapping at `key`, whose sibling
keys continue it; and a deeper line opens a block only as a collection — a value on the line below its key
opens none. These are the blocks YAML's scanner opens:

```pycon
>>> show(yamlish.layout(yamlish.tokenize("- a: b\n  c: d\nk:\n  v\n", eof=True)))
['-', 'IN', 'a', ':', 'b', 'NL', 'c', ':', 'd', 'NL', 'DE', 'k', ':', 'NL', 'v', 'NL', 'EOF']
```

A lexer with no such role lays out positionally. The free function takes them as `entry_kinds=` and
`value_kinds=`; in C++, `scilex::layout(tokens, mode_significant, lexer.layout_roles())`. In a `.lex` grammar the options are `indent=`,
`extend=` and `continue=` ({doc}`grammar-format`); in C++, `scilex::rule::indent`, `extend` and
`continuation`.

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
