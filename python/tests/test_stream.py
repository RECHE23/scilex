"""Lexer.stream(): text fed in pieces gives exactly tokenize's tokens -- munches, modes, positions,
lexeme types and errors -- whatever the cuts, and holds only what it has not returned."""
import os
import random
import unittest

import scilex

from _realpin import assert_real_pinned

_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# A modal grammar: a string with {…} interpolations that nest, beside plain code.
_CODE, _NAME, _NUM, _OPEN, _TEXT, _LB, _RB, _CLOSE = range(8)
_MODAL = [
    (99, r"[ \t\n]+", True, ["default", "interp"]),
    (_NAME, r"[A-Za-z_]\w*", False, ["default", "interp"]),
    (_NUM, r"[0-9]+", False, ["default", "interp"]),
    (_CODE, r"[-+*/=]", False, ["default", "interp"]),
    (_OPEN, r'f"', False, ["default", "interp"], ("push", "fstr")),
    (_TEXT, r'[^"{}]+', False, ["fstr"]),
    (_LB, r"\{", False, ["fstr", "interp"], ("push", "interp")),
    (_RB, r"\}", False, ["interp"], ("pop",)),
    (_CLOSE, r'"', False, ["fstr"], ("pop",)),
]
_MODAL_TEXT = 'x = f"a {y + f"b {z}"} c" + 42\nw = f"{1}"'


def fields(tokens):
    return [(t.kind, t.lexeme, t.offset, t.line, t.column, t.mode) for t in tokens]


def streamed(lexer, text, cuts):
    """The tokens of ``text`` fed as the pieces between ``cuts``, then finished."""
    stream = lexer.stream()
    out = []
    at = 0
    for cut in list(cuts) + [len(text)]:
        out += stream.feed(text[at:cut])
        at = cut
    out += stream.finish()
    return fields(out)


def sample_lexer():
    return scilex.load_grammar(os.path.join(_ROOT, "examples", "sample.lex")).lexer()


class StreamTests(unittest.TestCase):
    def setUp(self):
        assert_real_pinned(self)  # can_extend, which the stream asks, needs the pinned REAL

    def test_every_cut_in_two_gives_tokenize_tokens(self):
        cases = [
            (sample_lexer(), 'x = 41 + 1.5 # note\nname = "a b" <= (y, z)'),
            (scilex.Lexer(_MODAL), _MODAL_TEXT),
        ]
        for lexer, text in cases:
            whole = fields(lexer.tokenize(text))
            self.assertGreater(len(whole), 5)
            for cut in range(len(text) + 1):
                self.assertEqual(streamed(lexer, text, [cut]), whole, f"cut at {cut}")

    def test_random_small_pieces_give_tokenize_tokens(self):
        rng = random.Random(20260927)
        lexer = scilex.Lexer(_MODAL)
        whole = fields(lexer.tokenize(_MODAL_TEXT))
        for _ in range(200):
            cuts, at = [], 0
            while at < len(_MODAL_TEXT):
                at += rng.randint(0, 8)
                cuts.append(min(at, len(_MODAL_TEXT)))
            self.assertEqual(streamed(lexer, _MODAL_TEXT, cuts), whole, f"cuts {cuts}")

    def test_a_token_waits_only_while_more_text_could_change_it(self):
        # Not "every token but the last": a z makes one token of everything since the a.
        lexer = scilex.Lexer([(0, r"a[^z]*z", False), (1, r"[a-y]", False)])
        stream = lexer.stream()
        self.assertEqual([t.lexeme for t in stream.feed("xab")], ["x"])
        self.assertEqual(stream.feed(""), [])
        self.assertEqual(stream.feed("c"), [])  # the a may still open a[^z]*z
        self.assertEqual([t.lexeme for t in stream.feed("z")], ["abcz"])
        self.assertEqual(stream.finish(), [])

    def test_decided_tokens_are_returned_and_only_the_rest_is_held(self):
        lexer = sample_lexer()
        stream = lexer.stream()
        first = stream.feed("f(1, 22, 3")
        self.assertEqual([t.lexeme for t in first], ["f", "(", "1", ",", "22", ","])
        self.assertEqual(stream.buffered, 2)  # " 3": the 3 may still grow
        doc = "".join(f"name{i} = {i} + 2.5 # c\n" for i in range(2000))
        stream = lexer.stream()
        count, most = 0, 0
        for at in range(0, len(doc), 64):
            count += len(stream.feed(doc[at:at + 64]))
            most = max(most, stream.buffered)
        count += len(stream.finish())
        self.assertEqual(count, len(lexer.tokenize(doc)))
        self.assertLess(most, 128)  # a chunk and the token it ends inside, never the text so far

    def test_bytes_chunks_give_bytes_and_may_split_a_code_point(self):
        lexer = scilex.Lexer([(0, r"\w+", False), (1, r"\s+", True)])
        text = "café 変数 ok".encode("utf-8")
        whole = fields(lexer.tokenize(text))
        self.assertEqual([t[1] for t in whole], [b"caf\xc3\xa9", "変数".encode("utf-8"), b"ok"])
        for cut in range(len(text) + 1):
            self.assertEqual(streamed(lexer, text, [cut]), whole, f"cut at {cut}")

    def test_str_chunks_give_str_positions_in_bytes(self):
        lexer = scilex.Lexer([(0, r"\w+", False), (1, r"\s+", True)])
        text = "café\n変数 ok"
        self.assertEqual(streamed(lexer, text, [3, 5, 7]), fields(lexer.tokenize(text)))
        self.assertEqual(streamed(lexer, text, [3])[-1][2:5], (13, 2, 8))

    def test_chunk_type_is_fixed_by_the_first(self):
        lexer = sample_lexer()
        stream = lexer.stream()
        stream.feed("x ")
        with self.assertRaisesRegex(TypeError, "bytes after str"):
            stream.feed(b"y")
        stream = lexer.stream()
        stream.feed(b"x ")
        with self.assertRaisesRegex(TypeError, "str after bytes"):
            stream.feed("y")
        with self.assertRaises(TypeError):
            lexer.stream().feed(42)

    def test_feed_after_finish_and_finish_twice_raise(self):
        stream = sample_lexer().stream()
        stream.feed("x")
        self.assertEqual([t.lexeme for t in stream.finish()], ["x"])
        with self.assertRaisesRegex(scilex.error, "feed after finish"):
            stream.feed("y")
        with self.assertRaisesRegex(scilex.error, "finish called twice"):
            stream.finish()

    def test_finish_without_feed_gives_nothing(self):
        self.assertEqual(sample_lexer().stream().finish(), [])

    def test_errors_where_tokenize_raises_with_its_context(self):
        lexer = sample_lexer()
        text = "x = 1 +\n  y $ 2"
        with self.assertRaises(scilex.LexError) as whole:
            lexer.tokenize(text)
        for cut in range(len(text) + 1):
            stream = lexer.stream()
            with self.assertRaises(scilex.LexError, msg=f"cut at {cut}") as got:
                stream.feed(text[:cut])
                stream.feed(text[cut:])
                stream.finish()
            self.assertEqual(got.exception.position, whole.exception.position)
            # The stream raises once the text decides the error, which may be before the bytes after
            # it arrive: its snippet is tokenize's, cut after the offending byte at the latest.
            self.assertIn("‹$›", got.exception.context)
            self.assertTrue(whole.exception.context.startswith(got.exception.context))
            self.assertTrue(str(whole.exception).startswith(str(got.exception)))
            with self.assertRaisesRegex(scilex.error, "feed after finish"):
                stream.feed("z")  # the error ended the text

    def test_context_survives_the_text_the_stream_dropped(self):
        lexer = sample_lexer()
        text = "a " * 500 + "$"
        with self.assertRaises(scilex.LexError) as whole:
            lexer.tokenize(text)
        stream = lexer.stream()
        with self.assertRaises(scilex.LexError) as got:
            for at in range(0, len(text), 7):
                stream.feed(text[at:at + 7])
        self.assertEqual(str(got.exception), str(whole.exception))

    def test_an_unterminated_mode_is_reported_where_it_was_entered(self):
        lexer = scilex.Lexer(_MODAL)
        held = 'x = f"open ' + "t" * 300          # one text token: the entry is still held
        dropped = 'x = f"' + "a {1} " * 100       # returned tokens: the entry was dropped
        for text in (held, dropped):
            with self.assertRaises(scilex.LexError) as whole:
                lexer.tokenize(text)
            stream = lexer.stream()
            with self.assertRaises(scilex.LexError) as got:
                for at in range(0, len(text), 16):
                    stream.feed(text[at:at + 16])
                stream.finish()
            self.assertEqual(got.exception.position, whole.exception.position)
            if text is held:
                self.assertEqual(str(got.exception), str(whole.exception))
            else:
                # The position, and no snippet of bytes that are not the ones there.
                cause = str(whole.exception).split("; at line")[0]
                self.assertEqual(str(got.exception), cause + "; at line 1, column 5")
                self.assertFalse(hasattr(got.exception, "context"))

    def test_recovery_tokens_match_tokenize(self):
        lexer = scilex.load_grammar(os.path.join(_ROOT, "examples", "sample.lex")).lexer(errors="token")
        for text in ["x = $$ 1 @@", 'y = @"ab 1" + 2']:
            whole = fields(lexer.tokenize(text))
            self.assertIn(scilex.ERROR, [t[0] for t in whole])
            for cut in range(len(text) + 1):
                for second in range(cut, len(text) + 1):
                    self.assertEqual(streamed(lexer, text, [cut, second]), whole)

    def test_the_stream_keeps_its_lexer_alive(self):
        stream = scilex.Lexer([(0, r"[a-z]+", False)]).stream()
        self.assertEqual(stream.feed("abc"), [])  # the run may still grow
        self.assertEqual([t.lexeme for t in stream.finish()], ["abc"])


if __name__ == "__main__":
    unittest.main()
