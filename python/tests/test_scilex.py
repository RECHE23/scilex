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


def layout_lexer():
    # Indentation-significant sample: words, a colon, '#' line comments; all
    # whitespace skipped, so layout reconstructs structure from token positions.
    return scilex.Lexer([
        (0, r"\s+", True),       # whitespace (incl. newlines), skipped
        (3, r"#[^\n]*", True),   # line comment, skipped
        (2, r":", False),        # colon
        (1, r"\w+", False),      # word
    ])


def layout_kinds(text):
    laid = scilex.Layout().apply(layout_lexer().tokenize(text, eof=True))
    return [t.kind for t in laid]


class LayoutKindTests(unittest.TestCase):
    def test_reserved_kinds_are_int_min_plus_offsets(self):
        self.assertEqual(scilex.NEWLINE, -(2 ** 31) + 1)
        self.assertEqual(scilex.INDENT, -(2 ** 31) + 2)
        self.assertEqual(scilex.DEDENT, -(2 ** 31) + 3)

    def test_layout_exposes_the_kinds(self):
        self.assertEqual(scilex.Layout.newline_kind, scilex.NEWLINE)
        self.assertEqual(scilex.Layout.indent_kind, scilex.INDENT)
        self.assertEqual(scilex.Layout.dedent_kind, scilex.DEDENT)


class LayoutTests(unittest.TestCase):
    def test_basic_indent_and_dedent(self):
        # if x:
        #     a
        #     b
        # c
        nl, ind, ded, eof = scilex.NEWLINE, scilex.INDENT, scilex.DEDENT, scilex.END_OF_INPUT
        self.assertEqual(
            layout_kinds("if x:\n    a\n    b\nc"),
            [1, 1, 2, nl, ind, 1, nl, 1, nl, ded, 1, nl, eof],
        )

    def test_trailing_block_is_closed_at_eof(self):
        # A block still open at end-of-file is closed by a DEDENT before the terminal token.
        nl, ind, ded, eof = scilex.NEWLINE, scilex.INDENT, scilex.DEDENT, scilex.END_OF_INPUT
        self.assertEqual(layout_kinds("a\n    b"), [1, nl, ind, 1, nl, ded, eof])

    def test_comment_only_lines_add_no_structure(self):
        # The middle line has only a (skipped) comment: no token, so no NEWLINE for it.
        nl, eof = scilex.NEWLINE, scilex.END_OF_INPUT
        self.assertEqual(layout_kinds("a\n# just a comment\nb"), [1, nl, 1, nl, eof])

    def test_nested_indentation(self):
        nl, ind, ded, eof = scilex.NEWLINE, scilex.INDENT, scilex.DEDENT, scilex.END_OF_INPUT
        self.assertEqual(
            layout_kinds("a\n  b\n    c\nd"),
            [1, nl, ind, 1, nl, ind, 1, nl, ded, ded, 1, nl, eof],
        )

    def test_original_tokens_are_preserved(self):
        laid = scilex.Layout().apply(layout_lexer().tokenize("if x:\n    a", eof=True))
        words = [(t.lexeme, t.line, t.column) for t in laid if t.kind == 1]
        self.assertEqual(words, [("if", 1, 1), ("x", 1, 4), ("a", 2, 5)])

    def test_synthetic_tokens_have_empty_lexeme(self):
        laid = scilex.Layout().apply(layout_lexer().tokenize("a\n    b", eof=True))
        synthetic = {scilex.NEWLINE, scilex.INDENT, scilex.DEDENT}
        self.assertTrue(all(t.lexeme == "" for t in laid if t.kind in synthetic))

    def test_empty_input_yields_only_eof(self):
        laid = scilex.Layout().apply(layout_lexer().tokenize("", eof=True))
        self.assertEqual([t.kind for t in laid], [scilex.END_OF_INPUT])

    def test_inconsistent_dedent_raises_with_position(self):
        # 'b' dedents to column 3, which matches neither the outer (0) nor inner (4) level.
        with self.assertRaises(scilex.error) as caught:
            scilex.Layout().apply(layout_lexer().tokenize("if x:\n    a\n  b", eof=True))
        self.assertEqual(caught.exception.position.line, 3)
        self.assertEqual(caught.exception.position.column, 3)

    def test_module_level_layout(self):
        nl, eof = scilex.NEWLINE, scilex.END_OF_INPUT
        laid = scilex.layout(layout_lexer().tokenize("a\nb", eof=True))
        self.assertEqual([t.kind for t in laid], [1, nl, 1, nl, eof])


class ErrorContextTests(unittest.TestCase):
    def test_lexing_error_carries_context(self):
        with self.assertRaises(scilex.error) as caught:
            sample_lexer().tokenize("foo @ bar")
        exc = caught.exception
        self.assertEqual(exc.context, "foo ‹@› bar")  # byte fenced in ‹ ›
        self.assertIn("line 1, column 5", str(exc))
        self.assertIn("‹@›", str(exc))

    def test_context_at_start_of_input(self):
        with self.assertRaises(scilex.error) as caught:
            sample_lexer().tokenize("@x")
        self.assertEqual(caught.exception.context, "‹@›x")  # empty left window

    def test_context_clamps_the_window(self):
        with self.assertRaises(scilex.error) as caught:
            sample_lexer().tokenize("abcdefghijklmnop@")
        # ~8 bytes of left context are kept before the marked byte, then nothing right
        self.assertEqual(caught.exception.context, "ijklmnop‹@›")

    def test_context_survives_a_multibyte_boundary(self):
        # The error sits on 'é' (a 2-byte UTF-8 lead): byte-slicing the window must not
        # crash — a split codepoint decodes to the replacement character.
        lexer = scilex.Lexer([(0, r"\s+", True), (1, r"[a-z]+", False)])
        with self.assertRaises(scilex.error) as caught:
            lexer.tokenize("café")
        exc = caught.exception
        self.assertEqual(exc.position.offset, 3)   # the 'é' lead byte
        self.assertTrue(exc.context.startswith("caf"))
        self.assertIsInstance(exc.context, str)    # built without raising

    def test_context_in_eof_mode(self):
        with self.assertRaises(scilex.error) as caught:
            sample_lexer().tokenize("foo @", eof=True)
        self.assertIn("‹@›", caught.exception.context)

    def test_scan_error_carries_context(self):
        gen = sample_lexer().scan("foo @")
        self.assertEqual(next(gen).lexeme, "foo")   # valid token first
        with self.assertRaises(scilex.error) as caught:
            next(gen)
        self.assertIn("‹@›", caught.exception.context)

    def test_layout_error_has_position_but_no_context(self):
        # Layout.apply receives tokens, not the source, so a layout error carries a
        # position but no context snippet (the documented limitation).
        with self.assertRaises(scilex.error) as caught:
            scilex.Layout().apply(layout_lexer().tokenize("if x:\n    a\n  b", eof=True))
        self.assertEqual(caught.exception.position.line, 3)
        self.assertIsNone(getattr(caught.exception, "context", None))


# f-string grammar kinds (mirrors examples/fstring.hpp).
_WS, _IDENT, _NUM, _OP, _OPEN, _TEXT, _CLOSE, _LB, _RB = range(9)


def fstring_lexer():
    """A Python-style f-string lexer over default / fstr / interp modes."""
    code = ["default", "interp"]  # code rules are shared by top level and interps
    return scilex.Lexer([
        (_WS, r"\s+", True, code),
        (_IDENT, r"[A-Za-z_][A-Za-z0-9_]*", False, code),
        (_NUM, r"[0-9]+(\.[0-9]+)?", False, code),
        (_OP, r"[-+*/%=<>!&|^,.:;()]+", False, code),
        (_OPEN, r'f"', False, code, ("push", "fstr")),
        (_TEXT, r'\{\{|\}\}|[^{}"]+', False, ["fstr"]),
        (_LB, r"\{", False, ["fstr", "interp"], ("push", "interp")),  # nests: brace depth
        (_CLOSE, r'"', False, ["fstr"], ("pop",)),
        (_RB, r"\}", False, ["interp"], ("pop",)),
    ])


class ModeTests(unittest.TestCase):
    def test_three_tuple_rule_stays_valid(self):
        # Backward compat: a plain (kind, pattern, skip) rule needs no mode fields.
        lexer = scilex.Lexer([(0, r"\s+", True), (1, r"[a-z]+", False)])
        self.assertEqual([t.kind for t in lexer.tokenize("ab cd")], [1, 1])

    def test_canonical_fstring_flow(self):
        toks = fstring_lexer().tokenize('f"a{x}b"')
        self.assertEqual([t.kind for t in toks],
                         [_OPEN, _TEXT, _LB, _IDENT, _RB, _TEXT, _CLOSE])
        self.assertEqual(toks[3].lexeme, "x")  # x lexed by the SAME code IDENT rule

    def test_nesting_via_the_stack(self):
        # Same " and { delimiters, disambiguated only by the mode stack.
        toks = fstring_lexer().tokenize('f"{f"{x}"}"')
        self.assertEqual([t.kind for t in toks],
                         [_OPEN, _LB, _OPEN, _LB, _IDENT, _RB, _CLOSE, _RB, _CLOSE])

    def test_escaped_braces_munch_whole(self):
        toks = fstring_lexer().tokenize('f"{{x}}"')
        self.assertEqual([(t.kind, t.lexeme) for t in toks],
                         [(_OPEN, 'f"'), (_TEXT, "{{"), (_TEXT, "x"), (_TEXT, "}}"), (_CLOSE, '"')])

    def test_dict_nested_in_interpolation(self):
        # Brace depth via the stack: a dict { } inside an interpolation nests
        # another interp level, so the interpolation ends at the matching }.
        toks = fstring_lexer().tokenize('f"{ {x} }"')
        self.assertEqual([t.kind for t in toks],
                         [_OPEN, _LB, _LB, _IDENT, _RB, _RB, _CLOSE])

    def test_triple_quote_spans_newlines(self):
        # A dotall + lazy regex makes a triple-quoted string a single token, even
        # across newlines (the f-string sibling feature, regex not modes).
        lexer = scilex.Lexer([(0, r'(?s)""".*?"""', False), (1, r"\s+", True)])
        toks = lexer.tokenize('"""line1\nline2"""')
        self.assertEqual(len(toks), 1)
        self.assertEqual(toks[0].lexeme, '"""line1\nline2"""')

    def test_code_rules_are_shared_with_interpolations(self):
        # The NUMBER rule (a code rule) lexes inside an interpolation, unduplicated.
        toks = fstring_lexer().tokenize('f"a{1}b"')
        self.assertEqual([t.kind for t in toks],
                         [_OPEN, _TEXT, _LB, _NUM, _RB, _TEXT, _CLOSE])

    def test_unterminated_mode_reports_opening(self):
        with self.assertRaises(scilex.error) as caught:
            fstring_lexer().tokenize('f"abc')  # never closed (#3)
        self.assertEqual(caught.exception.position.offset, 0)

    def test_pop_at_root_raises(self):
        # A pop active in the default (root) mode has nothing to leave (#2).
        lexer = scilex.Lexer([(0, r"[a-z]+", False), (1, r"\)", False, [], ("pop",))])
        with self.assertRaises(scilex.error) as caught:
            lexer.tokenize("a)")
        self.assertEqual(caught.exception.position.offset, 1)

    def test_no_rule_in_mode_raises(self):
        # In 'num' only digits match; a letter there matches no active rule (#1).
        lexer = scilex.Lexer([(0, r"#", False, [], ("push", "num")),
                              (1, r"[0-9]+", False, ["num"])])
        with self.assertRaises(scilex.error) as caught:
            lexer.tokenize("#a")
        self.assertEqual(caught.exception.position.offset, 1)

    def test_scan_matches_tokenize_for_modes(self):
        # The lazy scan path threads the same per-scan mode stack as tokenize.
        lexer = fstring_lexer()
        source = 'f"a{x + 1}b"'
        self.assertEqual([t.kind for t in lexer.scan(source)],
                         [t.kind for t in lexer.tokenize(source)])

    def test_modal_rules_property_shape(self):
        lexer = fstring_lexer()
        self.assertEqual(lexer.rules[_WS][0:3], (_WS, r"\s+", True))
        self.assertEqual(lexer.rules[7], (_CLOSE, '"', False, ["fstr"], ("pop",)))

    def test_bare_str_in_mode_rejected(self):
        with self.assertRaises(TypeError):
            scilex.Lexer([(0, r"a", False, "default")])

    def test_unknown_action_rejected(self):
        with self.assertRaises(ValueError):
            scilex.Lexer([(0, r"a", False, ["m"], ("jump", "m"))])

    def test_push_without_target_rejected(self):
        with self.assertRaises(TypeError):
            scilex.Lexer([(0, r"a", False, [], ("push",))])

    def test_transition_into_empty_mode_rejected(self):
        # No rule lives in 'ghost', so the C++ builder refuses the grammar.
        with self.assertRaises(scilex.error):
            scilex.Lexer([(0, r"a", False, [], ("push", "ghost"))])


if __name__ == "__main__":
    unittest.main()
