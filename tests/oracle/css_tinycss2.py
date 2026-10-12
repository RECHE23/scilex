"""The css example grammar against tinycss2, over the style sheets of the documentation toolchain (the
Sphinx theme's and Sphinx's own, pinned in docs/requirements.txt) and of the running interpreter: each style
sheet must lex to the same tokens — where each starts and a coarse class (identifier, function, at-keyword,
hash, string, url, number, percentage, dimension, unicode range, a bracket, any other literal) — as tinycss2
gives, or the run fails below its floor.

Usage: css_tinycss2.py <scilex CLI> [file …]   (run with the interpreter that has tinycss2: make css-oracle)
tinycss2 nests a block's tokens inside it and gives no position for its closing bracket: a closing bracket is
compared by its class alone. Comments and whitespace are left out on both sides.
"""
import concurrent.futures
import glob
import os
import subprocess
import sys
import sysconfig

import tinycss2

from python_tokenize import unescape

# The share of style sheets that must lex identically.
FLOOR = 1.0
OURS = {"AT": "at", "HASH": "hash", "STRING": "string", "DIM": "dimension", "PCT": "percentage",
        "NUMBER": "number", "NAME": "ident", "URL": "url", "FUNCTION": "function", "URANGE": "unicode-range",
        "{": "open", "(": "open", "[": "open", "}": "close", ")": "close", "]": "close", ":": "literal",
        ";": "literal", ",": "literal", "DELIM": "literal"}
THEIRS = {"at-keyword": "at", "hash": "hash", "string": "string", "dimension": "dimension",
          "percentage": "percentage", "number": "number", "ident": "ident", "url": "url",
          "unicode-range": "unicode-range", "literal": "literal"}


def flatten(nodes, out):
    for node in nodes:
        if node.type in ("whitespace", "comment"):
            continue
        if node.type in ("() block", "[] block", "{} block", "function"):
            out.append(("function" if node.type == "function" else "open", node.source_line, node.source_column))
            flatten(node.arguments if node.type == "function" else node.content, out)
            out.append(("close", None, None))
        else:
            out.append((THEIRS.get(node.type, node.type), node.source_line, node.source_column))
    return out


def scilex_tokens(cli, path):
    """The CLI's tokens as (class, line, column in code points), a closing bracket without its position."""
    run = subprocess.run([cli, "--example", "css", "--columns=codepoints", path], capture_output=True)
    tokens = []
    for line in run.stdout.decode("utf-8", "surrogateescape").split("\n"):
        fields = line.split("\t")
        if len(fields) != 3 or line.startswith("#"):
            continue
        kind, _, position = fields
        row, column = (int(part) for part in position.split(":"))
        tokens.append(("close", None, None) if OURS[kind] == "close" else (OURS[kind], row, column))
    return tokens, run.stderr.decode("utf-8", "replace").strip()


def compare(cli, path):
    with open(path, "rb") as source:
        text = source.read().decode("utf-8", "replace")
    want = flatten(tinycss2.parse_component_value_list(text, skip_comments=True), [])
    got, error = scilex_tokens(cli, path)
    for index, (expected, actual) in enumerate(zip(want, got)):
        if expected != actual:
            return f"{path}: token {index}: tinycss2 {expected}, scilex {actual}"
    if len(want) != len(got):
        return f"{path}: tinycss2 {len(want)} tokens, scilex {len(got)}" + (f" ({error})" if error else "")
    return None


def style_sheets():
    """The .css files under the running interpreter's installation: its standard library and packages."""
    roots = {sysconfig.get_paths()["stdlib"], sysconfig.get_paths()["purelib"]}
    return sorted({path for root in roots for path in glob.glob(os.path.join(root, "**", "*.css"), recursive=True)})


def main():
    cli = sys.argv[1]
    files = sys.argv[2:] or style_sheets()
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
        failures = [failure for failure in pool.map(lambda path: compare(cli, path), files) if failure]
    for failure in failures[:20]:
        print(failure)
    share = 1 - len(failures) / max(1, len(files))
    print(f"css: {len(files) - len(failures)}/{len(files)} style sheets lex as tinycss2's ({share:.4f}, floor {FLOOR})")
    return 0 if files and share >= FLOOR else 1


if __name__ == "__main__":
    sys.exit(main())
