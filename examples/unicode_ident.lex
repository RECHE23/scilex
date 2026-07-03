# unicode_ident.lex — a CLI grammar whose identifier rule is a bare, un-pinned \w+, so it reads
# Unicode identifiers (café, 変数). Contrast with the ASCII pin `(?a)\w+` (see examples/sample.lex).
# Try it:
#     printf 'café = 変数 + 1' | scilex examples/unicode_ident.lex
WS	(?a)\s+	skip
IDENT	\w+
NUMBER	[0-9]+
OP	[-+*/=]
