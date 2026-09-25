# The `.lex` grammar format

A grammar file holds one rule per line: a name, a tab, the pattern, then optionally a tab and
space-separated options. Blank lines and lines whose first non-blank character is `#` are ignored,
and a trailing `\r` is dropped, so CRLF files read the same.

| option | effect |
| --- | --- |
| `skip` | matches are consumed but not emitted |
| `in=m1,m2` | the modes the rule is active in (without it: `default` only) |
| `push=m` | when the rule wins, enter mode `m`, remembering the current one |
| `set=m` | when the rule wins, replace the current mode with `m` |
| `pop` | when the rule wins, return to the mode below |

A rule fires at most one of `push=`, `set=` and `pop`. Its kind is its position among the rules,
counting from 0, and its name labels that kind.

```text
# A string mode entered by " and left by the next one.
WS      \s+         skip
STRING  "           push=str
TEXT    [^"\\]+     in=str
ESCAPE  \\.         in=str
END     "           in=str pop
IDENT   [A-Za-z_]\w*
```

(The separators are tabs; the columns above are aligned for reading.)

## Loading a grammar

The format has one parser, in the optional header `scilex/grammar.hpp`, which `scilex.hpp` does not
include:

```cpp
#include <scilex/grammar.hpp>

scilex::grammar     g {scilex::load_grammar("strings.lex")};   // or parse_grammar(text, origin)
const scilex::lexer lex {std::move(g.rules)};
// g.names[kind] is the name of each token kind
```

Python reaches the same parser:

```pycon
>>> import scilex
>>> g = scilex.parse_grammar('WS\t\\s+\tskip\nSTR\t"\tpush=str\nTXT\t[^"]+\tin=str\nEND\t"\tin=str pop\n'
...                          'ID\t[a-z]+\n')
>>> g.names
['WS', 'STR', 'TXT', 'END', 'ID']
>>> [(g.name(t.kind), t.lexeme) for t in g.lexer().tokenize('ab "hi" cd')]
[('ID', 'ab'), ('STR', '"'), ('TXT', 'hi'), ('END', '"'), ('ID', 'cd')]
```

`Grammar.lexer` takes `Lexer`'s options; `Grammar.rules` is the rule list `Lexer` takes.

## Errors

A malformed grammar raises `scilex::grammar_error` (Python: `scilex.GrammarError`), whose message reads
`origin:line:column: cause` — the form editors and compilers recognize — and whose `line`, `column`
and `cause` carry the parts. An invalid pattern is reported at the column of the offending character
in the line:

```pycon
>>> try:
...     scilex.parse_grammar("ID\t[a-z]+\nBAD\ta(\n", "my.lex")
... except scilex.GrammarError as error:
...     print(error.line, error.column)
...     print(error)
2 6
my.lex:2:6: invalid regex: missing ), unterminated subpattern
```

An unknown option, an empty mode name, a second transition, a line without a tab and a grammar with
no rules are refused the same way.
