"""SciLex micro-benchmarks versus Python's ``re`` — wall-time, honest, reproducible.

This is **not** a contest SciLex is built to win on raw throughput. ``re`` is a
mature C engine, and on benign inputs its backtracking matcher is typically faster
per byte than SciLex's per-position NFA scan reached through the abi3 binding.
SciLex's guarantee is a *different* one: **linear time, ReDoS-safe by construction**
(inherited from REAL), with maximal-munch lexer semantics over an ordered rule set.
The pathological case below is where that guarantee is the whole point.

Timing, statistics and the comparison gate come from SciForge's shared, dependency-free
benchmark substrate (sciforge.bench): the benign case goes through compare() — gated by a
token-count callable, with a bootstrap CI on the paired ratios it previously lacked — and the
linearity/scaling profiles use collect(). The substrate is never shipped; the sibling
../sciforge/python is on PYTHONPATH via the Makefile (make bench). Informational, never gated
(except a token-count mismatch, which is a real correctness failure and fails the run).
"""
import re
import sys
import time

import scilex
# Shared dep-free bench substrate; sibling on PYTHONPATH via the Makefile.
from sciforge.bench import collect, compare, median_iqr, verdict  # noqa: E402


def _median(label, call, samples=40):
    """Median per-call wall time of `call` (a single-fn profile, via the shared collector)."""
    return median_iqr(collect(label, call, samples=samples).samples)[0]


def benign_tokenization():
    """Throughput on an ordinary token soup — the everyday case (re is expected to win).

    A comparative case: SciLex (subject) vs re (reference), gated by token *count* (the two
    tokenizers must agree on how many tokens they emit). Returns the compare() Case.
    """
    source = "foo = bar + 42 * baz - 7 ; " * 400  # ~10 KB
    rules = [(0, r"\s+", True), (1, r"[0-9]+", False),
             (2, r"[A-Za-z_]\w*", False), (3, r"[-+*/=;]", False)]
    master = re.compile(r"(?P<WS>\s+)|(?P<NUM>[0-9]+)|(?P<ID>[A-Za-z_]\w*)|(?P<OP>[-+*/=;])")
    lexer = scilex.Lexer(rules)

    def with_scilex():
        return lexer.tokenize(source)

    def with_re():
        return [(m.lastgroup, m.group()) for m in master.finditer(source) if m.lastgroup != "WS"]

    # equal = the two token streams have the same length (count agreement). The judgement
    # lives here as a callable; sciforge.bench never knows what a token is.
    case = compare("benign tokenization", with_scilex, with_re,
                   lambda a, b: len(a) == len(b))
    count = len(with_scilex())
    print(f"  benign tokenization — {count} tokens, ~{len(source) // 1024} KB:")
    if case.extra["mismatch"]:
        print("    RESULT MISMATCH — the two tokenizers disagree on the token count")
        return case
    t_scilex = median_iqr(case.samples)[0]
    t_re = median_iqr(case.extra["reference_samples"])[0]
    ratio_median = median_iqr(case.extra["ratios"])[0]
    print(f"    scilex.Lexer.tokenize : {t_scilex * 1e3:8.3f} ms")
    print(f"    re.finditer (master)  : {t_re * 1e3:8.3f} ms   "
          f"(scilex is {t_scilex / t_re:.1f}x re here; ratio re/scilex {ratio_median:.2f})")
    return case


def redos_linearity():
    """``(a+)+b`` over a run of 'a' with no 'b': linear for SciLex, exponential for re."""
    lexer = scilex.Lexer([(1, r"(a+)+b", False), (2, r"a+", False)])
    pattern = re.compile(r"(a+)+b")
    print("  (a+)+b over n 'a' (never matches) — SciLex linear vs re backtracking:")
    print(f"    {'n':>4} {'scilex':>12} {'re.match':>14}")
    for n in (16, 18, 20, 22, 24, 26):
        text = "a" * n
        t_scilex = _median("redos", lambda t=text: lexer.tokenize(t), samples=30)
        start = time.perf_counter()
        pattern.match(text)  # catastrophic backtracking
        t_re = time.perf_counter() - start
        print(f"    {n:>4} {t_scilex * 1e6:9.2f} us {t_re * 1e3:11.2f} ms")
        if t_re > 1.0:
            print("    (re already past 1 s — stopping; its curve is exponential)")
            break
    big = "a" * 1000
    t_big = _median("redos-big", lambda: lexer.tokenize(big), samples=30)
    print(f"    n=1000: scilex {t_big * 1e6:.2f} us (still linear); re.match would never finish")


# A realistic source corpus and a growing keyword set, for the rule-count study that
# decides whether a first-byte dispatch in the lexer is worth implementing (tranche 3).
_SNIPPET = (
    "func compute(x, y) {\n"
    "    let total = 0;\n"
    "    for (i = 0; i < x + y; i = i + 1) {\n"
    '        total = total + i * 2 - 1; // accumulate\n'
    '        let name = "value_" + i;\n'
    "    }\n"
    "    return total;\n"
    "}\n"
)
_KEYWORDS = ["func", "let", "for", "return", "if", "else", "while", "break", "continue",
             "true", "false", "null", "int", "float", "string", "bool", "struct", "enum",
             "import", "export", "const", "static", "public", "private", "class", "new",
             "delete", "try", "catch", "throw", "switch", "case", "default", "do", "goto",
             "sizeof", "typedef", "union", "volatile", "register"]


def _realistic_lexer(n_keywords):
    """A realistic lexer: core rules + n_keywords literal keyword rules placed before
    the identifier rule (so a keyword wins the equal-length tie)."""
    rules = [
        (0, r"\s+", True),       # whitespace
        (1, r"//[^\n]*", True),  # line comment
        (2, r"[0-9]+", False),   # number
        (3, r'"[^"]*"', False),  # string
    ]
    rules += [(100 + i, kw, False) for i, kw in enumerate(_KEYWORDS[:n_keywords])]
    rules += [
        (4, r"[A-Za-z_]\w*", False),    # identifier
        (5, r"[-+*/=<>;(){},]", False),  # operators / punctuation
    ]
    return scilex.Lexer(rules), len(rules)


def _first_byte_sets(n_keywords):
    """The possible leading bytes of each rule — what a first-byte dispatch indexes on."""
    import string
    ws = set(" \t\n\r\f\v")
    digits = set(string.digits)
    letters = set(string.ascii_letters + "_")
    ops = set("-+*/=<>;(){},")
    sets = [ws, {"/"}, digits, {'"'}]
    sets += [{kw[0]} for kw in _KEYWORDS[:n_keywords]]
    sets += [letters, ops]
    return sets


def realistic_lexer_dispatch_study():
    """Data for the first-byte-dispatch question (tranche 3): how tokenization scales
    with rule count on a realistic lexer, and how much a dispatch would prune. Pure
    measurement — no lexer change."""
    source = _SNIPPET * 60  # ~15 KB of realistic source
    tokens = _realistic_lexer(40)[0].tokenize(source)
    print(f"  realistic lexer — rule-count scaling (~{len(source) // 1024} KB, "
          f"{len(tokens)} tokens; whitespace/comments skipped):")
    print(f"    {'rules':>6} {'tokenize':>11} {'us/token':>10}")
    points = []
    for n_kw in (0, 8, 16, 24, 32, 40):
        lexer, n_rules = _realistic_lexer(n_kw)
        t = _median("study", lambda lx=lexer: lx.tokenize(source), samples=20)
        points.append((n_rules, t))
        print(f"    {n_rules:>6} {t * 1e3:8.2f} ms {t / len(tokens) * 1e6:8.3f}")
    (r0, t0), (r1, t1) = points[0], points[-1]
    slope = (t1 - t0) / (r1 - r0)  # added time per extra rule, whole corpus
    print(f"    => ~{slope * 1e6:.1f} us per added rule over the corpus "
          f"({(t1 / t0):.1f}x slower at {r1} rules vs {r0})")
    # First-byte dispatch projection (static): avg rules whose leading byte can match a
    # position, vs total rules — the match-attempt reduction a dispatch would buy.
    sets = _first_byte_sets(40)
    total = len(sets)
    tried = sum(sum(1 for s in sets if ch in s) for ch in source) / len(source)
    print(f"    first-byte dispatch projection ({total}-rule lexer): avg rules whose "
          f"leading byte matches a position = {tried:.1f}/{total} -> ~{total / tried:.1f}x "
          f"fewer match attempts")


def main():
    print("SciLex benchmarks (median per-call via sciforge.bench; see BENCHMARKS.md):")
    benign = benign_tokenization()
    redos_linearity()
    realistic_lexer_dispatch_study()
    # The benign case is the one comparison; a count mismatch is a correctness failure.
    result = verdict([benign], subject_label="SciLex")
    if not result.passed:
        print(f"\nVERDICT: FAIL — {result.text}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
