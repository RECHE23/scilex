# Streaming

`tokenize` and `scan` need the whole text. A **stream** takes it in pieces — from a socket, a pipe, a
file read in blocks — and returns tokens as the text arrives: exactly the tokens `tokenize` gives the
whole text, with the same munches, modes, positions and errors, wherever the pieces are cut.

```pycon
>>> import scilex
>>> WORD, NUM = range(2)
>>> lexer = scilex.Lexer([(WORD, r"[a-z]+", False), (NUM, r"[0-9]+", False), (9, r"\s+", True)])
>>> stream = lexer.stream()
>>> [t.lexeme for t in stream.feed("abc 12")]
['abc']
>>> [t.lexeme for t in stream.feed("3 de")]
['123']
>>> [t.lexeme for t in stream.finish()]
['de']
```

`12` is not returned after the first piece: the next piece may carry more digits, and it does. A token
comes out once no text still to come can change it. That is not "every token but the last": a rule like
`a[^z]*z` makes one token of everything since the `a` the moment a `z` arrives, however many tokens
other rules made of that text meanwhile. So before each munch the stream asks every rule of the active
mode whether more text could change its match there — REAL's `can_extend` — and waits at the first
munch where one could. `finish` ends the text and lexes the rest.

```pycon
>>> quoted = scilex.Lexer([(0, r"a[^z]*z", False), (1, r"[a-y]", False)])
>>> stream = quoted.stream()
>>> [t.lexeme for t in stream.feed("xabc")]
['x']
>>> [t.lexeme for t in stream.feed("z")]
['abcz']
```

## Memory

The stream keeps only the text from the first token it has not returned; what it returned is dropped at
the next `feed`. Memory follows the longest token, not the input: a 72 KB JSON document fed in 64-byte
pieces never holds more than a piece and the token it ends inside. `buffered` is the count.

```pycon
>>> stream = lexer.stream()
>>> [t.lexeme for t in stream.feed("abc 12")]
['abc']
>>> stream.buffered
3
```

Positions stay in the whole text, in bytes, as `tokenize` gives them.

## Errors

An error is raised where `tokenize` raises it, once the text decides it: a byte no rule can begin is
reported as soon as it arrives. The stream is then finished; `feed` after `finish`, or `finish` twice,
is an error too. The message carries the position and the bytes around it, as far as they have arrived.
An unterminated mode names where the mode was entered even when the stream has dropped that text since;
the message then carries the position alone. Under {doc}`error recovery <errors>` an error run is one
token, returned once a rule matches after it or at the end.

## Types and threads

In Python every piece is of the first one's type: `str` pieces give `str` lexemes, `bytes` pieces give
`bytes` lexemes, and a `bytes` piece may end inside a UTF-8 sequence. A stream is a cursor: drive each
from one thread. `feed` holds the GIL.

In C++, `lexer::stream()` returns a `scilex::token_stream`. A token's lexeme views the stream's buffer
and is valid until the next `feed` or `finish`:

```cpp
scilex::token_stream in {lex.stream()};
while (read(chunk)) {
  for (const scilex::token& t : in.feed(chunk)) { use(t); }
}
for (const scilex::token& t : in.finish()) { use(t); }
```
