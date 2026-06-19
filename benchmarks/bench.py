"""SciLex micro-benchmarks versus Python's ``re`` — wall-time, honest, reproducible.

This is **not** a contest SciLex is built to win on raw throughput. ``re`` is a
mature C engine, and on benign inputs its backtracking matcher is typically faster
per byte than SciLex's per-position NFA scan reached through the abi3 binding.
SciLex's guarantee is a *different* one: **linear time, ReDoS-safe by construction**
(inherited from REAL), with maximal-munch lexer semantics over an ordered rule set.
The pathological case below is where that guarantee is the whole point.

Run with ``make bench`` (or ``python benchmarks/bench.py``). Best-of-N wall time,
minimum reported; informational only — never gated.
"""
import re
import time

import scilex


def best_time(call, iterations, repeats=5):
    """Minimum per-call wall time over ``repeats`` runs of ``iterations`` calls."""
    best = float("inf")
    for _ in range(repeats):
        start = time.perf_counter()
        for _ in range(iterations):
            call()
        best = min(best, time.perf_counter() - start)
    return best / iterations


def benign_tokenization():
    """Throughput on an ordinary token soup — the everyday case (re is expected to win)."""
    source = "foo = bar + 42 * baz - 7 ; " * 400  # ~10 KB
    rules = [(0, r"\s+", True), (1, r"[0-9]+", False),
             (2, r"[A-Za-z_]\w*", False), (3, r"[-+*/=;]", False)]
    master = re.compile(r"(?P<WS>\s+)|(?P<NUM>[0-9]+)|(?P<ID>[A-Za-z_]\w*)|(?P<OP>[-+*/=;])")
    lexer = scilex.Lexer(rules)

    def with_scilex():
        return lexer.tokenize(source)

    def with_re():
        return [(m.lastgroup, m.group()) for m in master.finditer(source) if m.lastgroup != "WS"]

    count = len(with_scilex())
    assert count == len(with_re()), "the two tokenizers must agree on the token count"
    t_scilex = best_time(with_scilex, iterations=200)
    t_re = best_time(with_re, iterations=200)
    print(f"  benign tokenization — {count} tokens, ~{len(source) // 1024} KB:")
    print(f"    scilex.Lexer.tokenize : {t_scilex * 1e3:8.3f} ms")
    print(f"    re.finditer (master)  : {t_re * 1e3:8.3f} ms   "
          f"(scilex is {t_scilex / t_re:.1f}x re here)")


def redos_linearity():
    """``(a+)+b`` over a run of 'a' with no 'b': linear for SciLex, exponential for re."""
    lexer = scilex.Lexer([(1, r"(a+)+b", False), (2, r"a+", False)])
    pattern = re.compile(r"(a+)+b")
    print("  (a+)+b over n 'a' (never matches) — SciLex linear vs re backtracking:")
    print(f"    {'n':>4} {'scilex':>12} {'re.match':>14}")
    for n in (16, 18, 20, 22, 24, 26):
        text = "a" * n
        t_scilex = best_time(lambda t=text: lexer.tokenize(t), iterations=50)
        start = time.perf_counter()
        pattern.match(text)  # catastrophic backtracking
        t_re = time.perf_counter() - start
        print(f"    {n:>4} {t_scilex * 1e6:9.2f} us {t_re * 1e3:11.2f} ms")
        if t_re > 1.0:
            print("    (re already past 1 s — stopping; its curve is exponential)")
            break
    big = "a" * 1000
    t_big = best_time(lambda: lexer.tokenize(big), iterations=50)
    print(f"    n=1000: scilex {t_big * 1e6:.2f} us (still linear); re.match would never finish")


def main():
    print("SciLex benchmarks (best-of-5 wall time, minimum reported; see BENCHMARKS.md):")
    benign_tokenization()
    redos_linearity()


if __name__ == "__main__":
    main()
