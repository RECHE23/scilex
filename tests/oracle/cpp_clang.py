"""The cpp example grammar against Clang's own lexer, over the headers of the C++ standard library Clang compiles
against: each header must lex to the same tokens — boundaries and a coarse class (name, number, character,
string, punctuator) — as `clang -cc1 -dump-raw-tokens` gives, or the run fails below its floor.

Usage: cpp_clang.py <scilex CLI> [file …]
A preprocessing directive is one token in the grammar (PREPROC), so Clang's tokens from a `#` that starts a
line up to the next line's first token are taken as one directive, ending at its last token, a comment
included, with the blanks and line splices after it left out on both sides (a line comment holds its own). Comments and whitespace outside directives are left out on both sides. The headers are found from `clang++ -E -v`; CLANG names the compiler.
"""
import concurrent.futures
import os
import re
import subprocess
import sys

from python_tokenize import unescape

# The share of headers that must lex identically. A raw string with a delimiter, R"d( … )d", is beyond the
# grammar (the closing sequence would have to repeat it); no header of the library has one.
FLOOR = 1.0

CLANG = os.environ.get("CLANG", "clang")
KINDS = {"KEYWORD": "name", "NAME": "name", "NUMBER": "number", "CHAR": "char", "STRING": "string",
         "OP": "punct", "PUNCT": "punct"}
DUMP = re.compile(r"^(\w+) '(.*?)'\t(.*?)\tLoc=<[^\n]*?:(\d+):(\d+)>$", re.M | re.S)
SKIPPED = {"unknown", "comment", "eof"}


def clang_class(kind):
    if kind == "raw_identifier":
        return "name"
    if kind == "numeric_constant":
        return "number"
    if kind.endswith("char_constant"):
        return "char"
    if kind.endswith("string_literal"):
        return "string"
    return "punct"


def trimmed(data, start, end):
    """`end` moved back over blanks and line splices: where a directive's last token ends."""
    while end > start and (data[end - 1] in b" \t\r\f\v" or data[end - 2:end] == b"\\\n"):
        end -= 2 if data[end - 2:end] == b"\\\n" else 1
    return end


def offsets(data):
    starts = [0] + [i + 1 for i, byte in enumerate(data) if byte == 10]
    return lambda line, column: starts[line - 1] + column - 1


def clang_tokens(path, data):
    """Clang's raw tokens as (start byte, end byte, class), a directive as one (…, "directive")."""
    run = subprocess.run([CLANG, "-cc1", "-dump-raw-tokens", "-std=c++26", "-x", "c++", path], capture_output=True)
    at = offsets(data)
    raw = [(m.group(1), at(int(m.group(4)), int(m.group(5))), "[StartOfLine]" in m.group(3))
           for m in DUMP.finditer(run.stderr.decode("utf-8", "surrogateescape"))]
    spans = []
    for index, (kind, start, _) in enumerate(raw):
        end = raw[index + 1][1] if index + 1 < len(raw) else len(data)
        spans.append((kind, start, end, raw[index][2]))
    tokens = []
    directive = None
    for kind, start, end, line_start in spans:
        if line_start and directive is not None:
            tokens.append((directive[0], trimmed(data, *directive[:2]), "directive"))
            directive = None
        if kind == "hash" and line_start:
            directive = (start, end, "directive")
        elif directive is not None:
            if kind != "unknown":  # a comment is part of its directive, which may go on after it
                directive = (directive[0], end, "directive")
        elif kind not in SKIPPED:
            tokens.append((start, end, clang_class(kind)))
    if directive is not None:
        tokens.append((directive[0], trimmed(data, *directive[:2]), "directive"))
    return tokens


def scilex_tokens(cli, path, data):
    """The CLI's tokens as (start byte, end byte, class), a directive's trailing blanks left out, and its error."""
    run = subprocess.run([cli, "--example", "cpp", path], capture_output=True)
    at = offsets(data)
    tokens = []
    for line in run.stdout.decode("utf-8", "surrogateescape").split("\n"):
        fields = line.split("\t")
        if len(fields) != 3 or line.startswith("#"):
            continue
        kind, lexeme, position = fields
        row, column = position.split(":")
        start = at(int(row), int(column))
        end = start + len(unescape(lexeme))
        if kind == "PREPROC":
            tokens.append((start, trimmed(data, start, end), "directive"))
        else:
            tokens.append((start, end, KINDS[kind]))
    return tokens, run.stderr.decode("utf-8", "replace").strip()


def compare(cli, path):
    with open(path, "rb") as source:
        data = source.read()
    want = clang_tokens(path, data)
    got, error = scilex_tokens(cli, path, data)
    for index, (expected, actual) in enumerate(zip(want, got)):
        if expected != actual:
            line = data.count(b"\n", 0, expected[0]) + 1
            return f"{path}:{line}: token {index}: clang {expected}, scilex {actual}"
    if len(want) != len(got):
        return f"{path}: clang {len(want)} tokens, scilex {len(got)}" + (f" ({error})" if error else "")
    return None


def headers():
    """Every file under the C++ standard library's include directories, as `clang++ -E -v` lists them."""
    run = subprocess.run([CLANG + "++" if CLANG == "clang" else CLANG, "-x", "c++", "-E", "-v", "-"],
                         input=b"", capture_output=True)
    listing = run.stderr.decode()
    inside = listing.split("#include <...> search starts here:")[1].split("End of search list.")[0]
    directories = [line.strip().split(" (")[0] for line in inside.strip().splitlines()]
    library = [d for d in directories if re.search(r"c\+\+", d)]
    files = []
    for directory in library:
        for root, _, names in os.walk(directory):
            files += [os.path.join(root, name) for name in sorted(names)]
    return files


def main():
    cli = sys.argv[1]
    files = sys.argv[2:] or headers()
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
        failures = [f for f in pool.map(lambda path: compare(cli, path), files) if f]
    for failure in failures[:20]:
        print(failure)
    share = 1 - len(failures) / max(1, len(files))
    print(f"cpp: {len(files) - len(failures)}/{len(files)} headers lex as Clang's ({share:.4f}, floor {FLOOR})")
    return 0 if files and share >= FLOOR else 1


if __name__ == "__main__":
    sys.exit(main())
