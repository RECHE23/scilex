"""Layout Awareness Level B from Python: the indent, extend and continuation fields of a rule tuple, the same
fields read from a .lex grammar (fragment and continue= included), their refusals, and an insignificant mode
that takes no part. The spans are PyYAML's on the same text."""
import unittest

import scilex

RULES = [
    (0, r"[ \t\r\n]+", True, ["default", "inner"]),
    (1, r"---|\.\.\.", False, [], None, None, None, "reset"),
    (2, r"-", False, [], None, None, None, "entry"),
    (3, r":", False, ["default", "inner"], None, None, None, "value"),
    (4, r"[|>]([1-9][+-]?|[+-][1-9]?)?", False, [], None, None, None, None, "block"),
    (5, r"[A-Za-z0-9_]+( +[A-Za-z0-9_]+)*", False, ["default", "inner"], None, None, None, "key", "lines",
     r"[A-Za-z0-9_\"-]+( +[A-Za-z0-9_\"-]+)*"),
    (6, r"\[", False, ["default", "inner"], ("push", "inner"), None, None, "key"),
    (7, r"\]", False, ["inner"], ("pop",)),
]

GRAMMAR = """\
WS\t[ \\t\\r\\n]+\tskip in=default,inner
%insignificant inner
DOC\t---|\\.\\.\\.\tindent=reset
DASH\t-\tindent=entry
COLON\t:\tin=default,inner indent=value
BLOCK\t[|>]([1-9][+-]?|[+-][1-9]?)?\textend=block
WORD\t[A-Za-z0-9_]+( +[A-Za-z0-9_]+)*\tin=default,inner indent=key extend=lines continue=MORE
MORE\t[A-Za-z0-9_"-]+( +[A-Za-z0-9_"-]+)*\tfragment
OPEN\t\\[\tin=default,inner push=inner indent=key
CLOSE\t\\]\tin=inner pop
"""

CASES = [
    ("a: |\n  one\n  two\n\nb: 1\n", ["a", "|\n  one\n  two\n\n", "b", "1"]),
    ("- k: |\n  j: v\n", ["k", "|\n", "j", "v"]),
    ("- name: foo\n  value: bar\n", ["name", "foo", "value", "bar"]),
    ("key:\n  long text\n  continued\n", ["key", "long text\n  continued"]),
    ("a: x\n  - y\n", ["a", "x\n  - y"]),
    ("a: [x\n  y]\n", ["a", "x", "y"]),
]


def bodies(lexer, text, block, word):
    return [t.lexeme for t in lexer.tokenize(text) if t.kind in (block, word)]


class LevelB(unittest.TestCase):
    def test_rule_tuples(self):
        lexer = scilex.Lexer(RULES, insignificant_modes=["inner"])
        for text, want in CASES:
            with self.subTest(text=text):
                self.assertEqual(bodies(lexer, text, 4, 5), want)
                streamed = lexer.stream()
                tokens = [t for i in range(len(text)) for t in streamed.feed(text[i])] + streamed.finish()
                self.assertEqual([t.lexeme for t in tokens if t.kind in (4, 5)], want)
        self.assertEqual(lexer.rules[4][7:], (None, "block", None))

    def test_grammar(self):
        grammar = scilex.parse_grammar(GRAMMAR)
        self.assertNotIn("MORE", grammar.names)
        lexer = grammar.lexer()
        block, word = grammar.names.index("BLOCK"), grammar.names.index("WORD")
        for text, want in CASES:
            with self.subTest(text=text):
                self.assertEqual(bodies(lexer, text, block, word), want)
        self.assertEqual(grammar.rules[word][7:9], ("key", "lines"))

    def test_an_insignificant_mode_takes_part_only_when_named(self):
        everywhere = scilex.Lexer(RULES)
        self.assertEqual(bodies(everywhere, "a: [x\n  y]\n", 4, 5), ["a", "x\n  y"])

    def test_refusals(self):
        with self.assertRaisesRegex(ValueError, "indent must be None or one of"):
            scilex.Lexer([(1, "a", False, [], None, None, None, "side")])
        with self.assertRaisesRegex(ValueError, "extend must be None or one of"):
            scilex.Lexer([(1, "a", False, [], None, None, None, None, "far")])
        with self.assertRaisesRegex(TypeError, "continuation must be None or a pattern str"):
            scilex.Lexer([(1, "a", False, [], None, None, None, None, "lines", 3)])
        with self.assertRaisesRegex(scilex.error, "skip rule"):
            scilex.Lexer([(1, " ", True, [], None, None, None, "key")])
        with self.assertRaises(scilex.GrammarError):
            scilex.parse_grammar("A\ta\tskip fragment\n")


if __name__ == "__main__":
    unittest.main()
