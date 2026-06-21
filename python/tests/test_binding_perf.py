"""Binding maturation tests (mirrors REAL): str+bytes input with the output type
following the input, the small/large GIL paths agreeing, a compile error carrying
the rule index + pattern offset, and thread-safety of the GIL-released scan."""
import threading
import unittest

import scilex

# Matches gil_release_collect_min_bytes in python/src/_scilex.cpp.
_GIL_THRESHOLD = 4096


def sample_lexer():
    return scilex.Lexer([
        (0, r"\s+", True),                      # whitespace, skipped
        (1, r"[0-9]+", False),                  # number
        (2, r"[A-Za-z_][A-Za-z0-9_]*", False),  # identifier
        (3, r"\+", False),                      # plus
    ])


class InputTypeTests(unittest.TestCase):
    def test_str_input_yields_str_lexemes(self):
        tokens = sample_lexer().tokenize("foo + 42")
        self.assertTrue(all(isinstance(t.lexeme, str) for t in tokens))
        self.assertEqual([(t.kind, t.lexeme) for t in tokens],
                         [(2, "foo"), (3, "+"), (1, "42")])

    def test_bytes_input_yields_bytes_lexemes(self):
        tokens = sample_lexer().tokenize(b"foo + 42")
        self.assertTrue(all(isinstance(t.lexeme, bytes) for t in tokens))
        self.assertEqual([(t.kind, t.lexeme) for t in tokens],
                         [(2, b"foo"), (3, b"+"), (1, b"42")])
        # Kinds and byte offsets match the str path exactly.
        str_tokens = sample_lexer().tokenize("foo + 42")
        self.assertEqual([(t.kind, t.offset) for t in tokens],
                         [(t.kind, t.offset) for t in str_tokens])

    def test_bytes_input_through_scan(self):
        tokens = list(sample_lexer().scan(b"ab 1"))
        self.assertEqual([(t.kind, t.lexeme) for t in tokens], [(2, b"ab"), (1, b"1")])

    def test_non_text_input_is_a_type_error(self):
        with self.assertRaises(TypeError):
            sample_lexer().tokenize(123)
        with self.assertRaises(TypeError):
            list(sample_lexer().scan(["not", "text"]))


class GilThresholdTests(unittest.TestCase):
    def test_small_and_large_subjects_agree(self):
        # The two GIL branches (held below 4 KB, released at/above it) must produce
        # the same maximal-munch result.
        lexer = sample_lexer()
        unit = "ab + 12 "      # 8 bytes -> 3 emitted tokens: [ident, plus, number]
        small = unit * 4       # 32 bytes   -> GIL held
        large = unit * 700     # 5600 bytes -> GIL released
        self.assertLess(len(small.encode()), _GIL_THRESHOLD)
        self.assertGreaterEqual(len(large.encode()), _GIL_THRESHOLD)
        self.assertEqual([t.kind for t in lexer.tokenize(small)], [2, 3, 1] * 4)
        self.assertEqual([t.kind for t in lexer.tokenize(large)], [2, 3, 1] * 700)


class CompileErrorTests(unittest.TestCase):
    def test_compile_error_reports_rule_index_and_offset(self):
        with self.assertRaises(scilex.error) as ctx:
            scilex.Lexer([(0, r"[a-z]+", False), (1, r"[bad", False)])
        message = str(ctx.exception)
        self.assertIn("rule 1", message)          # the failing rule's index
        self.assertIn("regex_error at", message)  # what() carries the pattern offset


class ConcurrencyTests(unittest.TestCase):
    def test_concurrent_tokenize_is_safe(self):
        # A shared lexer tokenized by many threads on a >4 KB subject (the GIL is
        # released during each scan): no crash, identical results. Validates that
        # releasing the GIL around a const, per-call-scratch match is sound.
        lexer = sample_lexer()
        subject = "ab + 12 " * 1000  # 8000 bytes -> GIL released
        expected = [(t.kind, t.lexeme, t.offset) for t in lexer.tokenize(subject)]
        results = []
        errors = []
        guard = threading.Lock()

        def worker():
            try:
                snapshot = [(t.kind, t.lexeme, t.offset) for t in lexer.tokenize(subject)]
            except Exception as exc:  # pragma: no cover - a failure here is the finding
                with guard:
                    errors.append(exc)
                return
            with guard:
                results.append(snapshot)

        threads = [threading.Thread(target=worker) for _ in range(8)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        self.assertEqual(errors, [])
        self.assertEqual(len(results), 8)
        self.assertTrue(all(snapshot == expected for snapshot in results))


class GuardTests(unittest.TestCase):
    def test_layout_rejects_bytes_lexemes(self):
        # layout is a str/indentation pass; a bytes-lexeme stream (from a bytes
        # source) is a clear TypeError, not a cryptic failure deep in the C layer.
        tokens = sample_lexer().tokenize(b"foo 42", eof=True)
        with self.assertRaises(TypeError):
            scilex.Layout().apply(tokens)

    def test_non_str_pattern_is_rejected(self):
        # A bytes (or other non-str) pattern is a clear TypeError, not a silent
        # str(b"abc") == "b'abc'" that would compile a nonsense regex.
        with self.assertRaises(TypeError):
            scilex.Lexer([(0, b"abc", False)])


if __name__ == "__main__":
    unittest.main()
