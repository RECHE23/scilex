# Errors and recovery

## What stops a scan

| cause | message begins | under `errors="token"` |
| --- | --- | --- |
| a byte no rule of the active mode can begin | `no rule matches in mode '…'` | an ERROR token |
| a winning match of length zero | `zero-length match in mode '…'` | still fatal |
| a pop at the root mode | `cannot pop the mode stack` | still fatal |
| a push past `max_mode_depth` | `cannot push another mode` | still fatal |
| input ending inside a pushed mode | `unterminated mode '…'` | a zero-width ERROR token at the end |

In C++ each is a `scilex::lex_error` whose `where()` is the position; in Python a `scilex.LexError`
(a subclass of `scilex.error`) with `.position` and, where the source is known, `.context` — a few
bytes either side of the offending one, fenced in `‹ ›`:

```pycon
>>> import scilex
>>> lx = scilex.Lexer([(0, r"\s+", True), (1, r"[a-z]+")])
>>> try:
...     lx.tokenize("abc @ def")
... except scilex.LexError as error:
...     print(error)
...     print(error.position.line, error.position.column, repr(error.context))
no rule matches in mode 'default' (entered at 1:1); at line 1, column 5: abc ‹@› def
1 5 'abc ‹@› def'
```

`scilex.LayoutError` and `scilex.GrammarError` are the other two subclasses: indentation that cannot be
laid out, and a malformed `.lex` grammar.

## Recovery

`errors="token"` (C++: `scilex::error_policy::token`) turns each maximal run of bytes no rule can begin
into one token of the reserved kind `ERROR`, holding exactly those bytes, and resumes where a rule
matches again:

```pycon
>>> lx = scilex.Lexer([(0, r"\s+", True), (1, r"[a-z]+")], errors="token")
>>> [(t.kind == scilex.ERROR, t.lexeme) for t in lx.tokenize("abc @#! def")]
[(False, 'abc'), (True, '@#!'), (False, 'def')]
```

The run stays in its mode and fires no transition. Finding where it ends costs a first-byte test per
position, so a noisy input stays cheap; a rule that scans far before failing is what makes recovery
expensive on a long run, which a definite leading byte avoids.
