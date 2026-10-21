#!/usr/bin/env python3
"""Drawn lexers against Python's ``re``: SciLex's tokenize over random rule sets and inputs.

The fixed benchmark (bench.py) times one benign token soup of four rules, where SciLex reads 2.1x ``re``. This draws
the case instead: a rule set (whitespace, then a draw of identifiers, keywords, integers, floats, hexadecimals,
double- and single-quoted strings, line and block comments, operators and Unicode identifiers, 3 to 42 rules in a
drawn order, keywords first as a user lists them so that a tie with a word goes to the keyword) and an input of its
tokens, from 64 bytes to LARGEST on a log scale, two of them run together only where they still lex apart. Each case
is lexed by SciLex (``Lexer.tokenize``) and by the master pattern a ``re`` user writes, through ``finditer``; and by
SciLex again without its DFA (``dfa="requested"``, no mode named), the per-rule engine a mode falls back on where no
DFA reproduces its rules, reported apart.

Answers first: SciLex's token stream must be the oracle's, the longest match at each position, a tie to the earlier
rule (SciLex's maximal munch), or the run fails. A master pattern takes the first alternative that matches, not the
longest, so it is ordered as a user orders it (keywords first, ending at a word's end; floats and hexadecimals before
integers; longer operators before shorter): where its stream still differs, the case is counted apart and not timed.
Each case is timed in turn, the two sides' batches alternating, the best batch kept; the ratio is re's time over
SciLex's: above 1, SciLex is faster.

    make bench-fuzz                       # 300 cases up to 64 KiB
    python3 benchmarks/bench_fuzz.py --cases 600 --largest 262144 --only 17
"""

import argparse
import json
import math
import platform
import random
import re
import sys
import time

import scilex
from sciforge.bench import ascii_ecdf, fmt, geomean_ci  # noqa: E402

KEYWORDS = ("if", "else", "elif", "while", "for", "in", "return", "def", "class", "let", "fn", "match", "case",
            "end", "do", "then", "assert", "as", "async", "await", "import", "from", "not", "and", "or")
OPERATORS = ("==", "=", "+=", "+", "-", "->", "*", "**", "/", "//", "<=", "<", ">=", ">", "!=", "(", ")", "{",
             "}", "[", "]", ",", ";", ":", "::", ".", "...", "%", "&&", "||", "!", "?", "@", "^", "~", "|", "&")
UNICODE_LETTERS = "éàüßλμπσΩжюд中文字"


def _word(rng, first, rest, longest=12):
    return rng.choice(first) + "".join(rng.choice(rest) for _ in range(rng.randrange(longest)))


LETTERS = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_"
DIGITS = "0123456789"


def _string(rng, quote):
    parts = []
    for _ in range(rng.randrange(24)):
        if rng.random() < 0.1:
            parts.append("\\" + rng.choice(quote + "\\nt"))
        else:
            parts.append(rng.choice(LETTERS + DIGITS + " .,:-"))
    return quote + "".join(parts) + quote


# Each family: (name, SciLex pattern, re pattern, a lexeme it matches).
FAMILIES = {
    "ident": (r"[A-Za-z_][A-Za-z0-9_]*", r"[A-Za-z_][A-Za-z0-9_]*", lambda rng: _word(rng, LETTERS, LETTERS + DIGITS)),
    "int": (r"[0-9]+", r"[0-9]+", lambda rng: _word(rng, DIGITS, DIGITS, 8)),
    "float": (r"[0-9]+\.[0-9]+([eE][-+]?[0-9]+)?", r"[0-9]+\.[0-9]+(?:[eE][-+]?[0-9]+)?",
              lambda rng: _word(rng, DIGITS, DIGITS, 4) + "." + _word(rng, DIGITS, DIGITS, 4)
              + (("e" + rng.choice("+-") + _word(rng, DIGITS, DIGITS, 2)) if rng.random() < 0.3 else "")),
    "hex": (r"0[xX][0-9a-fA-F]+", r"0[xX][0-9a-fA-F]+", lambda rng: "0x" + _word(rng, "0123456789abcdef",
                                                                                   "0123456789abcdefABCDEF", 8)),
    "string": (r'"([^"\\\n]|\\.)*"', r'"(?:[^"\\\n]|\\.)*"', lambda rng: _string(rng, '"')),
    "char": (r"'([^'\\\n]|\\.)*'", r"'(?:[^'\\\n]|\\.)*'", lambda rng: _string(rng, "'")),
    "line comment": (r"#[^\n]*", r"#[^\n]*", lambda rng: "#" + _string(rng, " ").strip() + "\n"),
    "block comment": (r"/\*([^*]|\*+[^*/])*\*+/", r"/\*(?:[^*]|\*+[^*/])*\*+/",
                      lambda rng: "/*" + _string(rng, " ").replace("*", "") + " */"),
    "unicode ident": (r"[\p{L}_][\p{L}\p{N}_]*", r"[^\W\d][\w]*",
                      lambda rng: _word(rng, UNICODE_LETTERS, UNICODE_LETTERS + LETTERS + DIGITS)),
}


def _longest(compiled, text, at):
    return max((m.end() - at for pattern in compiled if (m := pattern.match(text, at))), default=0)


def _splits(compiled, first, second):
    """Whether `first` written against `second` still lexes as the two of them."""
    joined = first + second
    return not first or (_longest(compiled, joined, 0) == len(first) and _longest(compiled, joined, len(first))
                         == len(second))


def draw_case(seed, index, largest):
    """The rule set and input of case `index`: the same for the same seed whatever else is drawn."""
    rng = random.Random(f"{seed}:{index}")
    families = rng.sample(sorted(FAMILIES), rng.randint(2, len(FAMILIES)))
    keywords = rng.sample(KEYWORDS, rng.randint(0, 12))
    operators = rng.sample(OPERATORS, rng.randint(0, 20))
    rules = []  # (kind, name, SciLex pattern, re pattern, generator)
    for name in families:
        scilex_pattern, re_pattern, generate = FAMILIES[name]
        rules.append([name, scilex_pattern, re_pattern, generate])
    for word in keywords:
        rules.append(["keyword " + word, re.escape(word), re.escape(word), lambda rng, w=word: w])
    for op in operators:
        rules.append(["operator " + op, re.escape(op), re.escape(op), lambda rng, o=op: o])
    rng.shuffle(rules)
    rules.sort(key=lambda rule: not rule[0].startswith("keyword"))  # keywords first: a tie with a word goes to them
    size = int(math.exp(rng.uniform(math.log(64), math.log(largest))))
    weights = [rng.random() + 0.05 for _ in rules]
    compiled = [re.compile(rule[2]) for rule in rules]
    pieces, length, previous = [], 0, ""
    while length < size:
        lexeme = rng.choices(rules, weights)[0][3](rng)
        separator = rng.choice((" ", " ", " ", "\n", "\t", "  ", ""))
        if not separator and not _splits(compiled, previous, lexeme):
            separator = " "  # run together, the two would lex otherwise (a float into the next, a word into a word)
        pieces += [separator, lexeme]
        length += len(separator) + len(lexeme)
        previous = lexeme
    text = "".join(pieces)
    return dict(index=index, rules=[(r[0], r[1], r[2]) for r in rules], text=text, size=len(text.encode()))


WHITESPACE = r"[ \t\r\n]+"


def by_hand(rules):
    """The master pattern's alternatives as a re user orders them, since re takes the first that matches: keywords
    first, each ending at a word's end where a word rule could take more, then hexadecimals and floats before
    integers, then the other families, then operators, the longer first. Each with its kind (its position in the
    rule set, from 1)."""
    def rank(item):
        kind, (name, _, _) = item
        if name.startswith("keyword"):
            return (0, kind)
        if name.startswith("operator"):
            return (3, -len(name), kind)
        return (1 if name in ("hex", "float") else 2, kind)
    ordered = sorted(enumerate(rules, 1), key=rank)
    words = any(name in ("ident", "unicode ident") for name, _, _ in rules)
    return [(kind, pattern + r"(?!\w)" if words and name.startswith("keyword") else pattern)
            for kind, (name, _, pattern) in ordered]


def lexers(case):
    """SciLex's Lexer, the same without a DFA, and the re master pattern for the case's rules, whitespace skipped
    (kind 0)."""
    rules = [(0, WHITESPACE, True)] + [(kind, pattern, False) for kind, (_, pattern, _) in
                                       enumerate(case["rules"], 1)]
    master = re.compile("|".join(f"(?P<k{kind}>{pattern})" for kind, pattern in by_hand(case["rules"]))
                        + f"|(?P<k0>{WHITESPACE})")
    return scilex.Lexer(rules), scilex.Lexer(rules, dfa="requested"), master


def oracle(case):
    """The longest match at each position, a tie to the earlier rule; None where no rule matches."""
    compiled = [re.compile(WHITESPACE)] + [re.compile(pattern) for _, _, pattern in case["rules"]]
    text, at, out = case["text"], 0, []
    while at < len(text):
        best, kind = 0, -1
        for k, pattern in enumerate(compiled):
            m = pattern.match(text, at)
            if m and m.end() - at > best:
                best, kind = m.end() - at, k
        if best == 0:
            return None
        if kind != 0:
            out.append((kind, text[at:at + best]))
        at += best
    return out


def with_scilex(lexer, text):
    return lexer.tokenize(text)


def with_re(master, text):
    return [(m.lastgroup, m.group()) for m in master.finditer(text) if m.lastgroup != "k0"]


def as_pairs(tokens):
    return [(t.kind, t.lexeme) for t in tokens]


def best_batch(call, rounds):
    """The best of `rounds` batches, each long enough (≥ 2 ms) for the clock: seconds per call."""
    number = 1
    while True:
        start = time.perf_counter()
        for _ in range(number):
            call()
        elapsed = time.perf_counter() - start
        if elapsed >= 2e-3:
            break
        number *= 2
    best = elapsed / number
    for _ in range(rounds - 1):
        start = time.perf_counter()
        for _ in range(number):
            call()
        best = min(best, (time.perf_counter() - start) / number)
    return best


def size_band(row):
    for bound, name in ((1 << 10, "< 1 KiB"), (1 << 13, "1-8 KiB"), (1 << 16, "8-64 KiB")):
        if row["size"] < bound:
            return name
    return ">= 64 KiB"


def rule_band(row):
    count = len(row["rules"]) + 1
    for bound, name in ((8, "3-7"), (16, "8-15"), (32, "16-31")):
        if count < bound:
            return name
    return "32+"


def table(title, groups):
    print(f"\n{title:<16} {'cases':>6} {'geomean':>9} {'95% CI':>17} {'worst':>8}")
    for name in sorted(groups):
        ratios = groups[name]
        if len(ratios) > 1:
            g, low, high = geomean_ci(ratios)
            ci = f"[{low:.2f}-{high:.2f}]"
        else:
            g, ci = ratios[0], ""
        print(f"{name:<16} {len(ratios):>6} {g:>8.2f}x {ci:>17} {min(ratios):>7.2f}x")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--cases", type=int, default=300)
    parser.add_argument("--seed", type=int, default=20261021)
    parser.add_argument("--largest", type=int, default=1 << 16, help="the largest input, in bytes")
    parser.add_argument("--rounds", type=int, default=3, help="batches per side in each of the two passes")
    parser.add_argument("--only", default="", help="comma-separated case indices (to reproduce a reported case)")
    parser.add_argument("--worst", type=int, default=15)
    parser.add_argument("--json", default="", help="write every case and its times here")
    args = parser.parse_args()

    indices = [int(i) for i in args.only.split(",")] if args.only else range(args.cases)
    started = time.perf_counter()
    rows, wrong, unlexable, disagree = [], [], 0, 0
    for index in indices:
        case = draw_case(args.seed, index, args.largest)
        expected = oracle(case)
        if expected is None:  # a lexeme run together with the next into no rule's text: drawn again never
            unlexable += 1
            continue
        lexer, per_rule, master = lexers(case)
        answers = []
        for each in (lexer, per_rule):
            try:
                answers.append(as_pairs(with_scilex(each, case["text"])))
            except scilex.error as caught:
                answers.append(repr(caught))
        if answers != [expected, expected] or per_rule.dfa_modes_active:
            wrong.append(case)
            continue
        theirs = [(int(group[1:]), lexeme) for group, lexeme in with_re(master, case["text"])]
        dfa = bool(lexer.dfa_modes_active)
        if theirs != expected:
            disagree += 1
            continue
        t_ours, t_rule, t_theirs = math.inf, math.inf, math.inf
        for _ in range(2):
            t_ours = min(t_ours, best_batch(lambda: with_scilex(lexer, case["text"]), args.rounds))
            t_rule = min(t_rule, best_batch(lambda: with_scilex(per_rule, case["text"]), args.rounds))
            t_theirs = min(t_theirs, best_batch(lambda: with_re(master, case["text"]), args.rounds))
        rows.append(dict(index=index, rules=case["rules"], size=case["size"], tokens=len(expected), dfa=dfa,
                         scilex=t_ours, per_rule=t_rule, re=t_theirs, ratio=t_theirs / t_ours,
                         per_rule_ratio=t_theirs / t_rule))

    print(f"SciLex {scilex.__version__} vs re (Python {platform.python_version()}), {platform.machine()}")
    print(f"{len(indices)} cases, seed {args.seed}, inputs of 64 bytes to {args.largest} on a log scale, "
          f"{time.perf_counter() - started:.0f} s")
    if wrong:
        print(f"\n{len(wrong)} case(s) where SciLex's tokens, with its DFA or without, are not the longest match's:")
        for case in wrong:
            print(f"  #{case['index']} {[name for name, _, _ in case['rules']]}")
    print(f"{unlexable} drawn input(s) no rule set could lex whole (skipped); {disagree} where the master pattern's "
          f"first match is not the longest (re's answer differs: not timed)")
    if not rows:
        return 1

    ratios = [row["ratio"] for row in rows]
    g, low, high = geomean_ci(ratios)
    faster = sum(r > 1 for r in ratios)
    print(f"\nre/SciLex over {len(rows)} cases: geometric mean {g:.2f}x [{low:.2f}-{high:.2f}], "
          f"SciLex faster in {faster} ({100 * faster / len(rows):.0f} %), the worst {min(ratios):.2f}x")
    per_rule_ratios = [row["per_rule_ratio"] for row in rows]
    g, low, high = geomean_ci(per_rule_ratios)
    faster = sum(r > 1 for r in per_rule_ratios)
    print(f"re/SciLex without its DFA: geometric mean {g:.2f}x [{low:.2f}-{high:.2f}], SciLex faster in {faster} "
          f"({100 * faster / len(rows):.0f} %), the worst {min(per_rule_ratios):.2f}x")
    for title, key in (("input", size_band), ("rules", rule_band),
                       ("engine", lambda r: "DFA" if r["dfa"] else "per rule")):
        groups = {}
        for row in rows:
            groups.setdefault(key(row), []).append(row["ratio"])
        table(title, groups)
    groups = {}
    for row in rows:
        for name in {name.split(" ")[0] if name.startswith(("keyword", "operator")) else name
                     for name, _, _ in row["rules"]}:
            groups.setdefault("with " + name, []).append(row["ratio"])
    table("family", groups)

    print("\nratios, cumulative (log scale; left of 1, re faster):")
    print(ascii_ecdf(ratios, log=True))

    print(f"\nthe {args.worst} worst cases (reproduce: --seed {args.seed} --largest {args.largest} --only N):")
    print(f"{'N':>5} {'ratio':>7} {'bytes':>8} {'tokens':>7} {'rules':>6} {'engine':<9} {'SciLex':>10} {'re':>10} "
          f"{'per rule':>10}")
    for row in sorted(rows, key=lambda r: r["ratio"])[:args.worst]:
        print(f"{row['index']:>5} {row['ratio']:>6.2f}x {row['size']:>8} {row['tokens']:>7} {len(row['rules']) + 1:>6} "
              f"{'DFA' if row['dfa'] else 'per rule':<9} {fmt(row['scilex'], 10)} {fmt(row['re'], 10)} "
              f"{fmt(row['per_rule'], 10)}")

    if args.json:
        with open(args.json, "w") as out:
            json.dump(dict(seed=args.seed, largest=args.largest, scilex=scilex.__version__,
                           machine=platform.machine(), rows=rows), out, indent=1)
    return 1 if wrong else 0


if __name__ == "__main__":
    sys.exit(main())
