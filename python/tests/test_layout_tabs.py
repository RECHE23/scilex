"""layout(tabs="python") against CPython's own tokenizer: the same INDENT / DEDENT / NEWLINE
sequence on every program, and the same refusal (TabError for tabs and spaces mixed ambiguously,
IndentationError for a dedent to no level) on every program CPython refuses."""
import io
import random
import sys
import tokenize
import unittest

import scilex

LEXER = scilex.Lexer([(0, r"[ \t\f\n]+", True), (1, r"[a-z][a-z0-9]*")])
KINDS = {scilex.NEWLINE: "NEWLINE", scilex.INDENT: "INDENT", scilex.DEDENT: "DEDENT"}
MIXED = "inconsistent use of tabs and spaces in indentation"


def ours(source):
    try:
        laid = LEXER.layout(LEXER.tokenize(source, eof=True), source=source, tabs="python")
    except scilex.error as exc:
        return ("TabError",) if str(exc).startswith(MIXED) else ("IndentationError",)
    return tuple(KINDS[t.kind] for t in laid if t.kind in KINDS)


def cpython(source):
    wanted = (tokenize.INDENT, tokenize.DEDENT, tokenize.NEWLINE)
    try:
        return tuple(tokenize.tok_name[t.type]
                     for t in tokenize.generate_tokens(io.StringIO(source).readline) if t.type in wanted)
    except TabError:
        return ("TabError",)
    except IndentationError:
        return ("IndentationError",)


@unittest.skipIf(sys.version_info < (3, 12), "tokenize applies the tab rule from 3.12 (the C tokenizer)")
class PythonTabPolicyAgreesWithCPython(unittest.TestCase):
    def test_random_programs(self):
        rng = random.Random(0x7AB5)  # fixed: a failure names the same program every run
        verdicts = {"accepted": 0, "TabError": 0, "IndentationError": 0}
        for n in range(4000):
            lines = ["a0"]
            for i in range(1, rng.randint(2, 7)):
                prefix = "".join(rng.choice(" \t") for _ in range(rng.randint(0, 9)))
                if rng.random() < 0.05:
                    prefix = rng.choice(" \t") + "\f" + prefix
                lines.append(f"{prefix}a{i}")
            source = "\n".join(lines) + "\n"
            want = cpython(source)
            self.assertEqual(ours(source), want, f"program {n}: {source!r}")
            verdicts[want[0] if want[0] in verdicts else "accepted"] += 1
        # Every class must be reached, or the sweep proves nothing about it.
        for verdict, count in verdicts.items():
            self.assertGreater(count, 100, f"{verdict}: {count} of 4000 -- {verdicts}")

    def test_columns_is_the_default_and_python_needs_the_source(self):
        source = "a\n\tb\n        c\n"
        tokens = LEXER.tokenize(source, eof=True)
        self.assertEqual(LEXER.layout(tokens), LEXER.layout(tokens, source=source, tabs="columns"))
        with self.assertRaises(ValueError):
            LEXER.layout(tokens, tabs="python")
        with self.assertRaises(ValueError):
            LEXER.layout(tokens, source=source, tabs="spaces")
        with self.assertRaises(ValueError):
            LEXER.layout(tokens, source="a", tabs="python")
        self.assertEqual(scilex.layout(tokens, source=source, tabs="columns"), LEXER.layout(tokens))


if __name__ == "__main__":
    unittest.main()
