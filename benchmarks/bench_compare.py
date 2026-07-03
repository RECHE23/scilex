#!/usr/bin/env python3
"""Cross-tool lexer comparison: SciLex against other tokenizers on the same real inputs.

Reproducible and honest. Every tool lexes the *same* corpus files with the *same* task (a full
tokenization pass), timed on the shared sciforge.bench statistics substrate (warmed, N samples,
median throughput with a bootstrap CI). Each number carries a comparability note, because the tools do
different amounts of work — this is stated, never hidden:

  - SciLex     : produces a token stream. Measured two ways — tokenize() (eager list) and scan() (lazy,
                 fully consumed). Linear-time / ReDoS-safe.
  - Pygments   : produces styled (token-type, text) pairs for highlighting — a superset of plain tokens.
  - tree-sitter: builds a full parse TREE (materially more work than tokenizing), and is incremental
                 (a capability not exercised here).
  - flex       : a compile-time code-generated DFA scanner — the raw-throughput ceiling for a fixed
                 grammar with a build step. Measured only if `flex` and a C compiler are present.
  - Logos / re2c: NOT measured here (Rust / a separate codegen toolchain); named for honesty.

Dependencies are optional: a tool that is not importable is skipped with a note, never a hard failure.
Numbers are informational — this harness is never wired into a gate. Run:

    python3 benchmarks/bench_compare.py                 # tables
    python3 benchmarks/bench_compare.py --json out.json # machine-readable, for the docs
"""

import argparse
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "python"))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "sciforge" / "python"))

import scilex
from sciforge.bench import bootstrap_ci, collect, median_iqr

DATA_DIR = pathlib.Path(__file__).resolve().parent / "data"


# --------------------------------------------------------------------------- corpus (deterministic)

def _make_json(target_bytes):
    """A deterministic, realistically-shaped JSON document of about target_bytes."""
    out = ['{"records":[']
    i = 0
    size = 0
    while size < target_bytes:
        rec = ('{{"id":{i},"name":"item-{i}","price":{p}.{c},"tags":["a","bb","ccc"],'
               '"active":{act},"note":"a short, escaped \\"string\\" value","nested":'
               '{{"x":{i},"y":-{i},"z":null}}}}').format(
                   i=i, p=i * 3, c=i % 100, act="true" if i % 2 else "false")
        out.append(("," if i else "") + rec)
        size += len(rec)
        i += 1
    out.append("]}")
    return "".join(out)


def _make_python(target_bytes):
    """A deterministic block of ordinary Python source of about target_bytes."""
    block = (
        "def process_{i}(items, factor={i}):\n"
        "    # accumulate a weighted total over the items\n"
        "    total = 0\n"
        "    for index, value in enumerate(items):\n"
        "        if value > {i} and value % 2 == 0:\n"
        "            total += value * factor - index\n"
        "        else:\n"
        "            total -= (value + {i}) / 3.5\n"
        "    label = 'result-{i}' if total > 0 else \"empty\"\n"
        "    return {{'id': {i}, 'total': total, 'label': label}}\n\n\n"
    )
    out = []
    size = 0
    i = 0
    while size < target_bytes:
        chunk = block.format(i=i)
        out.append(chunk)
        size += len(chunk)
        i += 1
    return "".join(out)


def corpus():
    """The corpus files, generated once and cached under benchmarks/data/ (versioned, reproducible)."""
    DATA_DIR.mkdir(exist_ok=True)
    specs = [("big.json", _make_json, 512 * 1024), ("sample.py", _make_python, 512 * 1024)]
    files = {}
    for name, maker, size in specs:
        path = DATA_DIR / name
        if not path.exists():
            path.write_text(maker(size))
        files[name] = path.read_text()
    return files


# --------------------------------------------------------------------------- SciLex grammars

def _scilex_json_lexer():
    ws, lb, rb, lk, rk, colon, comma, string, number, keyword = range(10)
    rules = [
        (ws, r"\s+", True),
        (lb, r"\{"), (rb, r"\}"), (lk, r"\["), (rk, r"\]"),
        (colon, r":"), (comma, r","),
        (string, r'"(\\.|[^"\\])*"'),
        (number, r"-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?"),
        (keyword, r"true|false|null"),
    ]
    return scilex.Lexer(rules, errors="token")


def _scilex_python_lexer():
    ws, comment, string, name, number, op, nl = range(7)
    rules = [
        (ws, r"[ \t]+", True),
        (comment, r"#[^\n]*", True),
        (string, r"\"(\\.|[^\"\\])*\"|'(\\.|[^'\\])*'"),
        (name, r"[A-Za-z_][A-Za-z0-9_]*"),
        (number, r"[0-9]+(\.[0-9]+)?"),
        (op, r"[-+*/%=<>!&|^~@,.:;()\[\]{}]"),
        (nl, r"\n", True),
    ]
    return scilex.Lexer(rules, errors="token")


SCILEX_LEXERS = {"big.json": _scilex_json_lexer, "sample.py": _scilex_python_lexer}
PYGMENTS_LEXERS = {"big.json": "json", "sample.py": "python"}
TREE_SITTER_LANGS = {"big.json": "tree_sitter_json", "sample.py": "tree_sitter_python"}


# --------------------------------------------------------------------------- tool runners

def scilex_runners(name, text):
    lex = SCILEX_LEXERS[name]()
    runners = {}
    obj_note = ("token stream as Python objects — the per-token binding cost is included; the C++ "
                "engine's own throughput, without the binding, is in BENCHMARKS.md and far higher")
    runners["scilex tokenize()"] = (lambda: lex.tokenize(text), "eager list; " + obj_note)

    def _scan():
        n = 0
        for _ in lex.scan(text):
            n += 1
        return n
    runners["scilex scan() (lazy)"] = (_scan, "lazy, fully consumed; " + obj_note)
    return runners


def pygments_runner(name, text):
    try:
        from pygments.lexers import get_lexer_by_name
    except ImportError:
        return None
    lexer = get_lexer_by_name(PYGMENTS_LEXERS[name])
    return (lambda: list(lexer.get_tokens(text)),
            "pure-Python; styled (token-type, text) pairs — a highlighting superset of plain tokens")


def tree_sitter_runner(name, text):
    try:
        import importlib
        from tree_sitter import Language, Parser
        mod = importlib.import_module(TREE_SITTER_LANGS[name])
    except ImportError:
        return None
    parser = Parser(Language(mod.language()))
    data = text.encode()
    return (lambda: parser.parse(data),
            "builds a full parse tree in C, returned as a handle — no per-token Python objects are "
            "materialised (walking the tree would add that); also incremental (not exercised)")


# --------------------------------------------------------------------------- flex (optional ceiling)

_FLEX_JSON = r"""
%option noyywrap noinput nounput
%{
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static long g_count;
%}
%%
[ \t\r\n]+      { }
\{              { g_count++; }
\}              { g_count++; }
\[              { g_count++; }
\]              { g_count++; }
:               { g_count++; }
,               { g_count++; }
\"(\\.|[^"\\])*\"   { g_count++; }
-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?  { g_count++; }
true|false|null { g_count++; }
.               { }
%%
int main(int argc, char** argv) {
    FILE* f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char* buf = malloc(n + 1); if (fread(buf, 1, n, f) != (size_t)n) return 2; buf[n] = 0; fclose(f);
    double best = 1e30;
    for (int it = 0; it < 30; it++) {
        g_count = 0;
        YY_BUFFER_STATE b = yy_scan_string(buf);
        struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
        yylex();
        clock_gettime(CLOCK_MONOTONIC, &t1);
        yy_delete_buffer(b);
        double s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
        if (s < best) best = s;
    }
    printf("{\"bytes\": %ld, \"best_seconds\": %.9f}\n", n, best);
    free(buf);
    return 0;
}
"""


def flex_json_ceiling(path):
    """Compile a flex JSON scanner and return its best-of-30 throughput, or None if unavailable."""
    if not shutil.which("flex"):
        return None
    cc = os.environ.get("CC") or shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
    if not cc:
        return None
    with tempfile.TemporaryDirectory() as d:
        lex_c = os.path.join(d, "j.c")
        binf = os.path.join(d, "j")
        try:
            subprocess.run(["flex", "-o", lex_c, "-"], input=_FLEX_JSON, text=True,
                           check=True, capture_output=True)
            subprocess.run([cc, "-O2", "-o", binf, lex_c], check=True, capture_output=True)
            out = subprocess.run([binf, str(path)], check=True, capture_output=True, text=True)
        except (subprocess.CalledProcessError, OSError):
            return None
    res = json.loads(out.stdout)
    return res["bytes"] / res["best_seconds"] / 1e6  # MB/s


# --------------------------------------------------------------------------- measurement + report

def throughput(text, fn):
    """Best-of-N (min-time) MB/s and its 95% bootstrap CI, from warmed samples (sciforge.bench)."""
    samples = collect("x", fn).samples
    best = median_iqr(samples)[4]           # the minimum time — best-of-N throughput
    lo, hi = bootstrap_ci(samples, min)     # CI of the best-of-N time
    nbytes = len(text.encode())
    return {"mbps": nbytes / best / 1e6,
            "mbps_lo": nbytes / hi / 1e6, "mbps_hi": nbytes / lo / 1e6}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--json", type=pathlib.Path, help="write machine-readable results here")
    args = ap.parse_args()

    files = corpus()
    report = {"files": {}, "notes": {
        "logos_re2c": "not measured (Rust / separate codegen toolchain)"}}

    for name, text in files.items():
        nbytes = len(text.encode())
        rows = []
        for label, (fn, note) in scilex_runners(name, text).items():
            rows.append((label, throughput(text, fn), note))
        py = pygments_runner(name, text)
        if py:
            rows.append(("Pygments", throughput(text, py[0]), py[1]))
        else:
            rows.append(("Pygments", None, "not installed"))
        ts = tree_sitter_runner(name, text)
        if ts:
            rows.append(("tree-sitter", throughput(text, ts[0]), ts[1]))
        else:
            rows.append(("tree-sitter", None, "not installed"))
        if name.endswith(".json"):
            fc = flex_json_ceiling(DATA_DIR / name)
            rows.append(("flex (codegen)",
                         {"mbps": fc, "mbps_lo": None, "mbps_hi": None} if fc else None,
                         "compile-time DFA scanner — the codegen ceiling"
                         if fc else "flex or a C compiler not available"))

        print("\n=== {} ({:,} bytes) ===".format(name, nbytes))
        print("  {:<22} {:>12}   {}".format("tool", "MB/s", "comparability note"))
        for label, r, note in rows:
            mbps = "{:8.1f}".format(r["mbps"]) if r and r["mbps"] else "     n/a"
            print("  {:<22} {:>12}   {}".format(label, mbps, note))
        report["files"][name] = {
            "bytes": nbytes,
            "rows": [{"tool": lb, "result": r, "note": nt} for lb, r, nt in rows]}

    if args.json:
        args.json.write_text(json.dumps(report, indent=2))
        print("\nwrote", args.json)


if __name__ == "__main__":
    main()
