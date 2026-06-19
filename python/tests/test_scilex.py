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


class ScanTests(unittest.TestCase):
    def test_scan_matches_tokenize(self):
        lexer = sample_lexer()
        self.assertEqual(list(lexer.scan("foo + 42")), lexer.tokenize("foo + 42"))

    def test_scan_is_a_lazy_generator(self):
        import types
        gen = sample_lexer().scan("a b c")
        self.assertIsInstance(gen, types.GeneratorType)
        self.assertEqual(next(gen).lexeme, "a")  # produced on demand, before the rest

    def test_scan_empty_input_yields_nothing(self):
        self.assertEqual(list(sample_lexer().scan("")), [])

    def test_scan_round_trips_unicode(self):
        lexer = scilex.Lexer([(1, r"\S+", False), (0, r"\s+", True)])
        self.assertEqual([t.lexeme for t in lexer.scan("café ok")], ["café", "ok"])

    def test_module_level_scan(self):
        tokens = list(scilex.scan([(1, r"[0-9]+", False)], "123"))
        self.assertEqual(tokens[0].lexeme, "123")


class EofTests(unittest.TestCase):
    def test_tokenize_eof_appends_end_of_input(self):
        tokens = sample_lexer().tokenize("42", eof=True)
        self.assertEqual(tokens[-1].kind, scilex.END_OF_INPUT)

    def test_tokenize_default_omits_eof(self):
        tokens = sample_lexer().tokenize("42")
        self.assertTrue(all(t.kind != scilex.END_OF_INPUT for t in tokens))

    def test_scan_eof_appends_end_of_input(self):
        tokens = list(sample_lexer().scan("42", eof=True))
        self.assertEqual(tokens[-1].kind, scilex.END_OF_INPUT)

    def test_eof_token_sits_at_the_end_position(self):
        eof = sample_lexer().tokenize("ab", eof=True)[-1]
        self.assertEqual(eof.kind, scilex.END_OF_INPUT)
        self.assertEqual(eof.offset, 2)   # past "ab"
        self.assertEqual(eof.lexeme, "")  # empty lexeme at the end

    def test_scan_eof_on_empty_input_yields_only_eof(self):
        tokens = list(sample_lexer().scan("", eof=True))
        self.assertEqual([t.kind for t in tokens], [scilex.END_OF_INPUT])

    def test_module_level_tokenize_eof(self):
        tokens = scilex.tokenize([(1, r"[0-9]+", False)], "1", eof=True)
        self.assertEqual(tokens[-1].kind, scilex.END_OF_INPUT)


class TokenStructureTests(unittest.TestCase):
    def test_token_has_a_structured_position(self):
        token = sample_lexer().tokenize("ab")[0]
        self.assertIsInstance(token.position, scilex.Position)
        self.assertEqual((token.position.offset, token.position.line, token.position.column),
                         (0, 1, 1))

    def test_offset_line_column_are_position_shortcuts(self):
        token = sample_lexer().tokenize("\nab")[0]  # leading newline is skipped whitespace
        self.assertEqual((token.offset, token.line, token.column), (1, 2, 1))
        self.assertEqual((token.offset, token.line, token.column),
                         (token.position.offset, token.position.line, token.position.column))

    def test_token_repr_shows_kind_lexeme_and_place(self):
        text = repr(sample_lexer().tokenize("foo")[0])
        self.assertIn("kind=2", text)
        self.assertIn("'foo'", text)
        self.assertIn("line=1", text)

    def test_token_equality_and_hash(self):
        first = sample_lexer().tokenize("foo")[0]
        second = sample_lexer().tokenize("foo")[0]
        self.assertEqual(first, second)
        self.assertEqual(hash(first), hash(second))
        self.assertNotEqual(first, sample_lexer().tokenize("bar")[0])

    def test_position_repr_equality_and_hash(self):
        position = scilex.Position(7, 2, 3)
        self.assertEqual(position, scilex.Position(7, 2, 3))
        self.assertNotEqual(position, scilex.Position(0, 1, 1))
        self.assertEqual(hash(position), hash(scilex.Position(7, 2, 3)))
        self.assertIn("line=2", repr(position))


class PositionedErrorTests(unittest.TestCase):
    def test_tokenize_error_carries_position(self):
        with self.assertRaises(scilex.error) as caught:
            sample_lexer().tokenize("foo @")
        error = caught.exception
        self.assertIsInstance(error.position, scilex.Position)
        self.assertEqual(error.position.offset, 4)  # '@' is the 5th byte
        self.assertEqual(error.position.column, 5)
        self.assertEqual(error.offset, 4)           # raw shortcuts present too

    def test_error_position_tracks_lines(self):
        with self.assertRaises(scilex.error) as caught:
            sample_lexer().tokenize("a\n@")
        self.assertEqual((caught.exception.position.line, caught.exception.position.column), (2, 1))

    def test_scan_raises_only_after_valid_tokens(self):
        gen = sample_lexer().scan("foo @")
        self.assertEqual(next(gen).lexeme, "foo")  # the valid token is yielded first
        with self.assertRaises(scilex.error) as caught:
            next(gen)
        self.assertEqual(caught.exception.position.offset, 4)

    def test_lexer_error_is_an_alias(self):
        self.assertIs(scilex.LexerError, scilex.error)
        with self.assertRaises(scilex.LexerError):
            sample_lexer().tokenize("@")


if __name__ == "__main__":
    unittest.main()
