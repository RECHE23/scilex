"""DFA-accelerated modes (Lexer dfa_modes): opt-in, the dfa_modes_active property, and
byte-identical tokens with the fast path on vs off — plus the silent fallback to the
regular engine when a mode is not DFA-able (an assertion) or not DFA-faithful (lazy)."""
import unittest

import scilex


def sql_rules():
    return [
        (0, r"\s+", True),                      # whitespace, skipped
        (1, r"select", False),                  # keyword
        (2, r"from", False),                    # keyword
        (3, r"[A-Za-z_][A-Za-z0-9_]*", False),  # identifier (overlaps keywords)
        (4, r"[0-9]+", False),                  # number
        (5, r"<>|<=|>=|[-+*/<>=]", False),      # operators
    ]


def fields(tokens):
    return [(t.kind, t.lexeme, t.offset, t.line, t.column) for t in tokens]


class DfaModesTests(unittest.TestCase):
    def test_default_mode_is_accelerated(self):
        lex = scilex.Lexer(sql_rules(), dfa_modes=("default",))
        self.assertIn("default", lex.dfa_modes_active)
        self.assertEqual(list(lex.dfa_modes), ["default"])

    def test_tokens_identical_dfa_on_vs_off(self):
        src = "select x from t where y >= 10 and z <> 0"
        off = scilex.Lexer(sql_rules())
        on = scilex.Lexer(sql_rules(), dfa_modes=("default",))
        self.assertEqual(fields(off.tokenize(src)), fields(on.tokenize(src)))
        self.assertGreater(len(on.tokenize(src)), 0)

    def test_unknown_dfa_mode_raises(self):
        with self.assertRaises(scilex.error):
            scilex.Lexer(sql_rules(), dfa_modes=("nonexistent",))

    def test_assertion_rule_falls_back_to_pike(self):
        rules = [(0, r"\s+", True), (1, r"end$", False), (2, r"[a-z]+", False)]
        lex = scilex.Lexer(rules, dfa_modes=("default",))
        self.assertNotIn("default", lex.dfa_modes_active)  # real::dfa_error -> Pike
        off = scilex.Lexer(rules)
        self.assertEqual(fields(off.tokenize("foo end")), fields(lex.tokenize("foo end")))

    def test_lazy_rule_falls_back_to_pike(self):
        rules = [(0, r"\s+", True), (1, r'(?s)""".*?"""', False), (2, r"[a-z]+", False)]
        lex = scilex.Lexer(rules, dfa_modes=("default",))
        self.assertNotIn("default", lex.dfa_modes_active)  # audit divergence -> Pike
        off = scilex.Lexer(rules)
        src = 'a """x""" b """y"""'
        self.assertEqual(fields(off.tokenize(src)), fields(lex.tokenize(src)))


if __name__ == "__main__":
    unittest.main()
