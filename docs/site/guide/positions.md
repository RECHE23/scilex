# Positions and columns

A token carries its kind, its lexeme — a view into the source, so a C++ caller keeps the source alive
— its start position, and the mode it was lexed in. A position is a byte offset, a 1-based line and a
1-based column; a newline starts the next line.

## Column units

The column counts **bytes** by default. `columns="codepoints"` counts Unicode scalar values and
`columns="utf16"` UTF-16 code units — the unit the Language Server Protocol uses, where an astral code
point counts two. The offset stays a byte offset in every unit.

```pycon
>>> import scilex
>>> rules = [(0, r"\s+", True), (1, r"\S+")]
>>> text = "é \U0001F600 x"
>>> [t.position.column for t in scilex.Lexer(rules).tokenize(text)]
[1, 4, 9]
>>> [t.position.column for t in scilex.Lexer(rules, columns="codepoints").tokenize(text)]
[1, 3, 5]
>>> [t.position.column for t in scilex.Lexer(rules, columns="utf16").tokenize(text)]
[1, 3, 6]
```

A malformed byte counts one column in every unit, so a position stays defined across the error runs
recovery emits. `Lexer.column_unit` (C++: `lexer::columns()`) says which unit a lexer counts in; a
position does not carry it.

## Where a token ends

Tokens carry their start only, so the stream costs nothing more for callers that never need the end.
`end_of(source, token)` computes it — exactly where the scan stood after the token, in the lexer's unit:

```pycon
>>> lx = scilex.Lexer(rules, columns="codepoints")
>>> tokens = lx.tokenize(text)
>>> lx.end_of(text, tokens[0])
Position(line=1, column=2, offset=2)
```

`é` is two bytes and one code point, so the token ends at byte 2, column 2; the skipped space lies
between it and the next token.

A token not lexed from the given source is refused (`std::invalid_argument` in C++, `ValueError` in
Python). A zero-width token — NEWLINE, INDENT, DEDENT, END_OF_INPUT — ends where it starts.
