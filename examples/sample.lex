# sample.lex — a thin SciLex grammar for the `scilex` CLI.
# Format: one rule per line   name<TAB>regex[<TAB>skip]   ('#' comments and blank lines ignored).
# Try it:
#     scilex examples/sample.lex <file>
#     echo 'x = 41 + 1' | scilex examples/sample.lex
# Order is only a tie-break: equal-length matches resolve to the earlier rule.

WS	\s+	skip
COMMENT	#[^\n]*	skip
NUMBER	[0-9]+(\.[0-9]+)?
# Identifiers — the author's Unicode-vs-DFA-speed choice (see the README):
#   ASCII, DFA-friendly (this rule):   IDENT   [A-Za-z_][A-Za-z0-9_]*
#   Unicode (café, 変数), general only: IDENT   [^\W\d]\w*     # \w is a code-point predicate: no DFA
IDENT	[A-Za-z_][A-Za-z0-9_]*
STRING	"[^"]*"
OP	<=|>=|==|!=|[-+*/%=<>]
PUNCT	[()\[\]{},;]
