"""Unit tests for the SciLex Python binding."""
import os
import unittest

import scilex


def sample_lexer():
    return scilex.Lexer([
        (0, r"\s+", True),                      # whitespace, skipped
        (1, r"[0-9]+", False),                  # number
        (2, r"[A-Za-z_][A-Za-z0-9_]*", False),  # identifier
        (3, r"\+", False),                      # plus
    ])


class TokenizeTests(unittest.TestCase):
    def test_kinds_and_lexemes(self):
        tokens = sample_lexer().tokenize("foo + 42")
        self.assertEqual([(t.kind, t.lexeme) for t in tokens],
                         [(2, "foo"), (3, "+"), (1, "42")])

    def test_skips_whitespace_and_tracks_byte_positions(self):
        tokens = sample_lexer().tokenize("a  bb")
        self.assertEqual([(t.lexeme, t.offset, t.column) for t in tokens],
                         [("a", 0, 1), ("bb", 3, 4)])

    def test_tracks_lines(self):
        tokens = sample_lexer().tokenize("a\nb")
        self.assertEqual([(t.lexeme, t.line) for t in tokens], [("a", 1), ("b", 2)])

    def test_two_arg_rule_defaults_skip_to_false(self):
        tokens = scilex.Lexer([(1, r"[0-9]+")]).tokenize("42")
        self.assertEqual(tokens[0].kind, 1)

    def test_unicode_lexeme_round_trips(self):
        lexer = scilex.Lexer([(1, r"\S+", False), (0, r"\s+", True)])
        self.assertEqual([t.lexeme for t in lexer.tokenize("café ok")], ["café", "ok"])

    def test_module_level_tokenize(self):
        tokens = scilex.tokenize([(1, r"[0-9]+", False)], "123")
        self.assertEqual(tokens[0].lexeme, "123")

    def test_rules_property_round_trips(self):
        self.assertEqual(scilex.Lexer([(7, r"x")]).rules, [(7, "x", False)])


class ErrorTests(unittest.TestCase):
    def test_unlexable_input_raises(self):
        with self.assertRaises(scilex.error):
            scilex.Lexer([(1, r"[0-9]+", False)]).tokenize("abc")

    def test_invalid_pattern_raises(self):
        with self.assertRaises(scilex.error):
            scilex.Lexer([(1, r"[", False)])


class PackagingTests(unittest.TestCase):
    def test_get_include_locates_headers(self):
        include = scilex.get_include()
        self.assertTrue(os.path.isfile(os.path.join(include, "scilex", "scilex.hpp")))

    def test_get_config(self):
        config = scilex.get_config()
        self.assertEqual(config["cxx_standard"], "c++20")
        self.assertEqual(config["version"], scilex.__version__)
        self.assertEqual(config["include"], scilex.get_include())

    def test_end_of_input_constant(self):
        self.assertEqual(scilex.END_OF_INPUT, -(2 ** 31))


if __name__ == "__main__":
    unittest.main()
