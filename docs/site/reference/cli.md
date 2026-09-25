# Command line

```text
scilex --list                      the built-in example grammars
scilex --example <lang> [file|-]   lex with a built-in grammar (its bundled sample without a file)
scilex <grammar.lex> [file|-]      lex with your grammar (standard input without a file)
scilex --check                     run every built-in grammar's self-check
scilex --version                   SciLex's version and the REAL it was built with
```

| option | effect |
| --- | --- |
| `--layout` | add NEWLINE / INDENT / DEDENT |
| `--errors=token` | turn unlexable runs into ERROR tokens instead of stopping |
| `--columns=codepoints`, `--columns=utf16` | count columns in that unit (default: bytes) |

Output is one token per line: the kind's name, a tab, the lexeme, a tab, `line:column`. A malformed
grammar is reported as `file:line:column: cause` and a lexical error as `lex error at line:column:
cause`, both on standard error with a non-zero exit status. The grammar format is in
{doc}`../guide/grammar-format`.
