# The `.lex` grammar format

A grammar file holds one rule per line: a name, a tab, the pattern, then optionally a tab and
space-separated options. Blank lines and lines whose first non-blank character is `#` are ignored,
a byte order mark that starts the file is dropped, and a trailing `\r` too, so CRLF files read the same. The spaces and tabs that end a line are
dropped too, so a pattern never ends in a blank: write a final space `[ ]` or `\x20`.

| option | effect |
| --- | --- |
| `skip` | matches are consumed but not emitted |
| `in=m1,m2` | the modes the rule is active in (without it: `default` only) |
| `push=m` | when the rule wins, enter mode `m`, remembering the current one |
| `set=m` | when the rule wins, replace the current mode with `m` |
| `pop` | when the rule wins, return to the mode below |
| `after=A,B` | active only right after a token of the rules named `A` or `B` (`^`: at the start) |
| `not_after=A,B` | active except right after such a token ({doc}`modes`, "The token before") |
| `line_start` | active only at the start of a line: the start of the text, or right after a `\n` |
| `not_line_start` | active only past the start of a line ({doc}`modes`, "The start of a line") |
| `indent=entry`, `key`, `value`, `reset` | the rule's part in the indentation the lexer tracks ({doc}`layout`, "Level B") |
| `extend=block`, `extend=lines` | how its token extends over the lines indented past its block ({doc}`layout`) |
| `continue=N` | with `extend=lines`: the pattern each continued line holds, the fragment (else the rule) named `N` |
| `fragment` | the line names a pattern for `continue=`; it is not a rule, has no kind, and takes no other option |

A rule fires at most one of `push=`, `set=` and `pop`, and takes at most one of `after=` and `not_after=`,
whose names stand for every rule bearing them, earlier or later in the file, and at most one of `line_start` and
`not_line_start`. A mode that `push=` or `set=` enters must
have a rule active in it (`in=`; the rules without `in=` are in `default`). Its kind is its position among the rules,
fragments not counted, from 0, and its name labels that kind.

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

## Directives

A line whose first non-blank character is `%` is a directive, not a rule, so it takes no kind and
leaves the rules' positions alone. The one directive is `%insignificant m1,m2`: the modes whose tokens
carry no layout structure, which the lexer built from the grammar takes as its `insignificant_modes`
(a bracketed region that a line break inside does not end, say). Each mode it names must have a rule
active in it, and it may repeat. A rule therefore cannot be named with a leading `%`.

```text
%insignificant bracket
OPEN    \(          push=bracket
CLOSE   \)          in=bracket pop
NEWLINE \n          in=default,bracket
NAME    [a-z]+      in=default,bracket
```

`grammar::insignificant_modes` (Python: `Grammar.insignificant_modes`) holds them; `Grammar.lexer()` uses
them unless it is given its own.

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

An unknown option, role or extension, an empty mode name, a second transition, a name no rule bears in `after=` or
`not_after=`, a `continue=` on a rule without `extend=lines` or naming nothing, a fragment with another option, a
`push=` or `set=` into a mode no rule is active in, an unknown directive, an `%insignificant` with no mode or a mode no rule is active in, a line
without a tab and a grammar with no rules are refused the same way.
