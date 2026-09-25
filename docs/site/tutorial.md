# Tutorial

Three steps, each building on the last: a flat JSON lexer, a string mode with escapes, and
significant indentation. The examples are Python; the C++ rules are the same data
({doc}`reference/cpp`), and each grammar is also a `.lex` file ({doc}`guide/grammar-format`).

## 1. A flat grammar: JSON

A rule is `(kind, pattern)`, plus `True` to skip what it matches. The longest match wins, and among
equally long matches the earlier rule does.

```pycon
>>> import scilex
>>> WS, LBRACE, RBRACE, LBRACKET, RBRACKET, COLON, COMMA, STRING, NUMBER, TRUE, FALSE, NULL = range(12)
>>> json = scilex.Lexer([
...     (WS, r"\s+", True),
...     (LBRACE, r"\{"), (RBRACE, r"\}"), (LBRACKET, r"\["), (RBRACKET, r"\]"),
...     (COLON, r":"), (COMMA, r","),
...     (STRING, r'"(?:[^"\\]|\\.)*"'),
...     (NUMBER, r"-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?"),
...     (TRUE, r"true"), (FALSE, r"false"), (NULL, r"null"),
... ])
>>> [t.lexeme for t in json.tokenize('{"a": [1, true]}')]
['{', '"a"', ':', '[', '1', ',', 'true', ']', '}']
```

Every rule here is one a DFA reproduces exactly, so the lexer runs this mode on a DFA:

```pycon
>>> json.dfa_modes_active
['default']
>>> json.pike_rules()   # no rule stays on the per-rule path
[]
```

A byte no rule matches raises `scilex.LexError`, which carries the position:

```pycon
>>> try:
...     json.tokenize('{"a": @}')
... except scilex.LexError as error:
...     print(error.position.line, error.position.column)
1 7
```

## 2. A string mode

One regex per string is fine for JSON, but a lexer often wants the string's parts: its text and its
escapes as separate tokens. A **mode** is a set of rules active together; a rule names the modes it
is active in and may push, pop or replace the mode when it wins.

```pycon
>>> WS, ID, OPEN, TEXT, ESCAPE, CLOSE = range(6)
>>> strings = scilex.Lexer([
...     (WS, r"\s+", True),
...     (ID, r"[a-z]+"),
...     (OPEN, r'"', False, [], ("push", "str")),         # enter the string
...     (TEXT, r'[^"\\]+', False, ["str"]),
...     (ESCAPE, r"\\.", False, ["str"]),
...     (CLOSE, r'"', False, ["str"], ("pop",)),          # back to the default mode
... ])
>>> [(t.kind, t.lexeme, t.mode) for t in strings.tokenize(r'say "a\"b" now')]
[(1, 'say', 'default'), (2, '"', 'default'), (3, 'a', 'str'), (4, '\\"', 'str'),
 (3, 'b', 'str'), (5, '"', 'str'), (1, 'now', 'default')]
```

An empty mode list means the `default` mode. Each token records the mode it was lexed in. Input that
ends inside a pushed mode is an error (`unterminated mode 'str'`), and so is a pop at the root.

## 3. Significant indentation

`Lexer.layout` turns line structure into NEWLINE, INDENT and DEDENT tokens, from the columns of each
line's first token. It needs the stream to end with `END_OF_INPUT`:

```pycon
>>> src = "if x\n  y\nz\n"
>>> lx = scilex.Lexer([(0, r"[ \t]+", True), (1, r"\n", True), (2, r"[a-z]+")])
>>> kinds = {scilex.NEWLINE: "NEWLINE", scilex.INDENT: "INDENT", scilex.DEDENT: "DEDENT",
...          scilex.END_OF_INPUT: "EOF"}
>>> [kinds.get(t.kind, t.lexeme) for t in lx.layout(lx.tokenize(src, eof=True))]
['if', 'x', 'NEWLINE', 'INDENT', 'y', 'NEWLINE', 'DEDENT', 'z', 'NEWLINE', 'EOF']
```

A mode can be declared **insignificant** — inside brackets, say — so its lines continue the current
one instead of shaping indentation; `tabs="python"` measures tabs as CPython does. Both are in
{doc}`guide/layout`.
