#!/usr/bin/env python3
"""Reporter for the SciLex C++ engine throughput collector (bench_lex.cpp).

The C++ binary only measures and emits the canonical sciforge.bench JSON (one Case per
measurement, raw per-call seconds, grouped into four sections by a domain field); this script
reads it back with sciforge.bench and prints the four tables. The headline throughput is
bytes / 1e6 / min(samples) (MB/s, the documented best-of-N minimum), so the numbers stay
comparable to the prior table. Pure timing: no pass/fail criterion, always exits 0.

  python benchmarks/bench_lex.py path/to/bench_lex    # runs the binary
  python benchmarks/bench_lex.py --json lex.json       # reads a saved run
Run via: make bench-lex  (manual; not a CI gate).
"""

import argparse
import json
import subprocess
import sys

from sciforge.bench import run_from_json

GRAMMAR_ORDER = ["json", "python", "cpp", "sql", "css", "lisp", "math", "xml", "yaml"]

# The two engine regimes, reported separately (no single aggregate): grammars whose shorthands are
# pinned ASCII (a byte-level class) vs grammars that keep Unicode text-mode shorthands (code-point
# predicates). Which of these actually accelerate under the DFA is a measured fact — see render_dfa.
ASCII_PINNED = ["json", "cpp", "sql", "css", "lisp", "math"]
TEXT_MODE = ["python", "xml", "yaml"]


def load(args):
    if args.json:
        with open(args.json, encoding="utf-8") as handle:
            return run_from_json(json.load(handle))
    out = subprocess.run([args.binary], capture_output=True, text=True, check=True).stdout
    return run_from_json(json.loads(out))


def _mbps(case):
    return case.extra["bytes"] / 1e6 / min(case.samples)


def _section(cases, name):
    return [c for c in cases if c.extra["section"] == name]


def render_grammar(cases, compiler):
    rows = _section(cases, "grammar")
    if not rows:
        return
    print("SciLex C++ engine throughput — per grammar, PIKE engine "
          "(best-of-N min, raw samples → sciforge.bench)")
    print("steady-state input: each grammar's sample scaled to >= 256 KiB")
    print(f"(the Pike per-rule + first-byte-dispatch engine across all grammars; the DFA opt-in is below)  [{compiler}]")
    print()
    by_grammar = {}
    for case in rows:
        by_grammar.setdefault(case.extra["grammar"], {})[case.extra["path"]] = case

    def segment(title, order):
        print(f"  {title}")
        print(f"  {'grammar':<8} {'KiB':>8} {'tokens':>9} {'eager MB/s':>13} {'lazy MB/s':>13}")
        for grammar in order:
            paths = by_grammar.get(grammar)
            if not paths:
                continue
            eager, lazy = paths["eager"], paths["lazy"]
            print(f"  {grammar:<8} {eager.extra['bytes'] // 1024:>8} {eager.extra['tokens']:>9} "
                  f"{_mbps(eager):>13.2f} {_mbps(lazy):>13.2f}")

    segment("ASCII-pinned shorthands (byte-level classes):", ASCII_PINNED)
    print()
    segment("Unicode text-mode shorthands (code-point predicates):", TEXT_MODE)


def render_linearity(cases):
    rows = _section(cases, "linearity")
    if not rows:
        return
    print("\nlinearity — cpp grammar at growing sizes (flat MB/s => linear time):")
    print(f"  {'KiB':>8} {'eager MB/s':>13}")
    for case in sorted(rows, key=lambda c: c.extra["bytes"]):
        print(f"  {case.extra['bytes'] // 1024:>8} {_mbps(case):>13.2f}")


def render_mode(cases):
    rows = _section(cases, "mode-overhead")
    if not rows:
        return
    print("\nmode overhead — python on its sample (modal vs a mono-mode baseline):")
    print(f"  {'variant':<10} {'KiB':>8} {'tokens':>9} {'eager MB/s':>13}")
    by_variant = {c.extra["variant"]: c for c in rows}
    for variant in ["modal", "mono-mode"]:
        case = by_variant.get(variant)
        if case is None:
            continue
        print(f"  {variant:<10} {case.extra['bytes'] // 1024:>8} {case.extra['tokens']:>9} "
              f"{_mbps(case):>13.2f}")


def render_dfa(cases):
    rows = _section(cases, "dfa-modes")
    if not rows:
        return
    print("\nDFA modes — full token path (tokenize), DFA-accelerated vs Pike:")
    print(f"  {'gram':<6} {'KiB':>8} {'tokens':>9} {'Pike MB/s':>13} {'DFA MB/s':>13} "
          f"{'speedup':>9} {'build us':>11} {'active':>7}")
    by_grammar = {}
    for case in rows:
        by_grammar.setdefault(case.extra["grammar"], {})[case.extra["path"]] = case
    order = ["json", "sql", "css", "math", "xml", "lisp", "yaml", "py*"]
    for grammar in order:
        paths = by_grammar.get(grammar)
        if not paths:
            continue
        # most grammars name the paths pike/dfa/build; the py* control names them off/on (no build).
        pike = paths.get("pike") or paths.get("off")
        dfa = paths.get("dfa") or paths.get("on")
        build = paths.get("build")
        active = pike.extra["active"]
        active_str = "accel" if active else "Pike (rejected)"
        build_str = f"{min(build.samples) * 1e6:.1f}" if build else "-"
        print(f"  {grammar:<6} {pike.extra['bytes'] // 1024:>8} {pike.extra['tokens']:>9} "
              f"{_mbps(pike):>13.2f} {_mbps(dfa):>13.2f} "
              f"{min(pike.samples) / min(dfa.samples):>8.1f}x {build_str:>11} {active_str:>16}")


def render_failure_cost(cases):
    rows = _section(cases, "failure-cost")
    if not rows:
        return
    by_name = {c.extra["path"] + "/" + c.extra.get("corpus", ""): c for c in rows}
    throw = next((c for c in rows if c.extra["path"] == "throw-isolated"), None)
    per_throw = min(throw.samples) / throw.extra["throws"] if throw else 0.0
    print("\nfailure-cost — the recover-and-resync loop on adversarial input (per rejected position):")
    print(f"  exception mechanism alone: {per_throw * 1e9:.0f} ns per throw+catch "
          f"(the per-byte cost an in-lexer recovery avoids)")
    print(f"  {'path':<6} {'corpus':<16} {'KiB':>5} {'rejected':>9} {'raw ns/pos':>11} {'net ns/pos':>11}")
    for key, case in by_name.items():
        if case.extra["path"] in ("throw-isolated", "non-fail-fast"):
            continue
        fails = case.extra["failures"]
        if fails == 0:
            print(f"  {case.extra['path']:<6} {case.extra['corpus']:<16} "
                  f"{case.extra['bytes'] // 1024:>5} {'0 (tolerated)':>9}")
            continue
        raw = min(case.samples) / fails * 1e9
        net = (min(case.samples) - fails * per_throw) / fails * 1e9
        print(f"  {case.extra['path']:<6} {case.extra['corpus']:<16} {case.extra['bytes'] // 1024:>5} "
              f"{fails:>9} {raw:>11.0f} {net:>11.0f}")
    nff = next((c for c in rows if c.extra["path"] == "non-fail-fast"), None)
    if nff:
        fails = nff.extra["failures"]
        net = (min(nff.samples) - fails * per_throw) / fails * 1e9 if fails else 0
        print(f"  non-fail-fast greedy rule on a no-terminator run ({nff.extra['bytes'] // 1024} KiB): "
              f"{net:.0f} ns/pos net — O(remaining) per position (a first-byte prefilter would skip it)")


def main():
    parser = argparse.ArgumentParser(description="SciLex C++ engine throughput reporter")
    parser.add_argument("binary", nargs="?", help="path to the bench_lex binary to run")
    parser.add_argument("--json", metavar="PATH", help="read a saved run instead of running the binary")
    args = parser.parse_args()
    if not args.binary and not args.json:
        parser.error("need a binary path or --json PATH")
    run = load(args)

    compiler = run.meta.get("compiler", "?")
    render_grammar(run.cases, compiler)
    render_linearity(run.cases)
    render_mode(run.cases)
    render_dfa(run.cases)
    render_failure_cost(run.cases)
    return 0


if __name__ == "__main__":
    sys.exit(main())
