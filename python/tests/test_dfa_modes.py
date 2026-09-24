"""DFA-accelerated modes (Lexer dfa_modes): opt-in, the dfa_modes_active property, and
byte-identical tokens with the fast path on vs off — plus the silent fallback to the
regular engine when a mode is not DFA-able (an assertion) or not DFA-faithful (lazy)."""
import unittest

import scilex

from _realpin import assert_real_pinned


def sql_rules():
    return [
        (0, r"(?a)\s+", True),                      # whitespace, skipped
        (1, r"select", False),                  # keyword
        (2, r"from", False),                    # keyword
        (3, r"[A-Za-z_][A-Za-z0-9_]*", False),  # identifier (overlaps keywords)
        (4, r"[0-9]+", False),                  # number
        (5, r"<>|<=|>=|[-+*/<>=]", False),      # operators
    ]


def fields(tokens):
    return [(t.kind, t.lexeme, t.offset, t.line, t.column) for t in tokens]


class DfaModesTests(unittest.TestCase):
    def setUp(self):
        assert_real_pinned(self)  # a stale REAL would silently demote these DFA modes

    def test_default_mode_is_accelerated(self):
        lex = scilex.Lexer(sql_rules(), dfa_modes=("default",))
        self.assertIn("default", lex.dfa_modes_active)
        self.assertEqual(list(lex.dfa_modes), ["default"])

    def test_tokens_identical_dfa_on_vs_off(self):
        src = "select x from t where y >= 10 and z <> 0"
        off = scilex.Lexer(sql_rules(), dfa="requested")
        on = scilex.Lexer(sql_rules(), dfa_modes=("default",))
        self.assertEqual(fields(off.tokenize(src)), fields(on.tokenize(src)))
        self.assertGreater(len(on.tokenize(src)), 0)

    def test_unknown_dfa_mode_raises(self):
        with self.assertRaises(scilex.error):
            scilex.Lexer(sql_rules(), dfa_modes=("nonexistent",))

    def test_assertion_rule_falls_back_to_pike(self):
        rules = [(0, r"(?a)\s+", True), (1, r"end$", False), (2, r"[a-z]+", False)]
        lex = scilex.Lexer(rules, dfa_modes=("default",))
        self.assertEqual(lex.pike_rules("default"), [1])  # only `end$` stays on Pike (dfa_error)
        off = scilex.Lexer(rules, dfa="requested")
        self.assertEqual(fields(off.tokenize("foo end")), fields(lex.tokenize("foo end")))

    def test_lazy_rule_falls_back_to_pike(self):
        rules = [(0, r"(?a)\s+", True), (1, r'(?s)""".*?"""', False), (2, r"[a-z]+", False)]
        lex = scilex.Lexer(rules, dfa_modes=("default",))
        self.assertEqual(lex.pike_rules("default"), [1])  # match() stops at the first """ -> Pike
        off = scilex.Lexer(rules, dfa="requested")
        src = 'a """x""" b """y"""'
        self.assertEqual(fields(off.tokenize(src)), fields(lex.tokenize(src)))


    def test_auto_is_the_default_and_requested_can_switch_it_off(self):
        self.assertIn("default", scilex.Lexer(sql_rules()).dfa_modes_active)
        self.assertEqual(scilex.Lexer(sql_rules(), dfa="requested").dfa_modes_active, [])
        with self.assertRaises(ValueError):
            scilex.Lexer(sql_rules(), dfa="always")

if __name__ == "__main__":
    unittest.main()
