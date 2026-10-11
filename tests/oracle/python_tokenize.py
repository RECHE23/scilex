"""The python example grammars against CPython's own tokenizer, over every source file of the running
interpreter's standard library: each file must lex to the same tokens — boundaries and a coarse class (name,
number, string, operator, f-string start / text / end) — as `tokenize` gives, or the run fails below its floor.

Usage: python_tokenize.py <scilex CLI> [file …]
Layout tokens (NEWLINE, INDENT, DEDENT) are left out: the layout pass is tested on its own. Adjacent f-string
text tokens are merged on both sides, since where a text run is cut (at a `{{`, a quote) carries no meaning.
Needs Python 3.12 or later, whose tokenizer splits f-strings into their parts as the grammar does.
"""
import concurrent.futures
import io
import os
import subprocess
import sys
import sysconfig
import tokenize

# The share of files that must lex identically, per grammar, set so that one more file below it fails: the
# ASCII grammar reads no non-ASCII identifier (six test files hold one), and one file is deliberately invalid.
FLOORS = {"python-unicode": 0.999, "python": 0.996}

KINDS = {"KEYWORD": "NAME", "NAME": "NAME", "NUMBER": "NUMBER", "STRING": "STRING", "TSTRING": "STRING",
         "FSTR_OPEN": "FSTART", "FSTR_TEXT": "FMID", "FSTR_CLOSE": "FEND"}
LAYOUT = {"NEWLINE", "INDENT", "DEDENT", "EOF"}
ESCAPES = {"\\": b"\\", "t": b"\t", "n": b"\n", "r": b"\r"}


def unescape(lexeme):
    """The bytes of a lexeme as the CLI prints it (`\\t`, `\\n`, `\\r`, `\\\\`, `\\xHH` escaped)."""
    out = bytearray()
    i = 0
    while i < len(lexeme):
        if lexeme[i] != "\\":
            out += lexeme[i].encode("utf-8", "surrogateescape")
            i += 1
        elif lexeme[i + 1] == "x":
            out.append(int(lexeme[i + 2:i + 4], 16))
            i += 4
        else:
            out += ESCAPES[lexeme[i + 1]]
            i += 2
    return bytes(out)


def merge_text(tokens):
    out = []
    for start, end, kind in tokens:
        if out and kind == "FMID" and out[-1][2] == "FMID" and out[-1][1] == start:
            out[-1] = (out[-1][0], end, kind)
        else:
            out.append((start, end, kind))
    return out


def scilex_tokens(cli, grammar, path, data):
    """The CLI's tokens as (start byte, end byte, class), and its error line if it stopped."""
    run = subprocess.run([cli, "--example", grammar, path], capture_output=True)
    line_starts = [0] + [i + 1 for i, byte in enumerate(data) if byte == 10]
    tokens = []
    for line in run.stdout.decode("utf-8", "surrogateescape").split("\n"):
        if not line or line.startswith("#"):
            continue
        kind, lexeme, position = line.split("\t")
        if kind in LAYOUT:
            continue
        row, column = map(int, position.split(":"))
        start = line_starts[row - 1] + column - 1
        tokens.append((start, start + len(unescape(lexeme)), KINDS.get(kind, "OP")))
    error = run.stderr.decode(errors="replace").strip().splitlines()[0] if run.returncode else None
    return merge_text(tokens), error


def python_tokens(data):
    """tokenize's tokens as (start byte, end byte, class); a leading byte-order mark is not a token."""
    bom = 3 if data.startswith(b"\xef\xbb\xbf") else 0
    text = data[bom:].decode("utf-8")
    lines = text.splitlines(keepends=True)
    starts = [bom]
    for line in lines:
        starts.append(starts[-1] + len(line.encode("utf-8")))

    def offset(row_column):
        row, column = row_column
        if row - 1 >= len(lines):
            return starts[-1]
        return starts[row - 1] + len(lines[row - 1][:column].encode("utf-8"))

    tokens = []
    for tok in tokenize.generate_tokens(io.StringIO(text).readline):
        name = tokenize.tok_name[tok.type]
        if name in ("NAME", "NUMBER", "STRING", "OP"):
            kind = name
        elif name in ("FSTRING_START", "TSTRING_START"):
            kind = "FSTART"
        elif name in ("FSTRING_MIDDLE", "TSTRING_MIDDLE"):
            kind = "FMID"
        elif name in ("FSTRING_END", "TSTRING_END"):
            kind = "FEND"
        else:
            continue
        if kind == "FMID" and tok.start == tok.end:
            continue
        tokens.append((offset(tok.start), offset(tok.end), kind))
    # tokenize gives an escaped `{{` or `}}` as one brace, its span short by the other: give the text its byte.
    for i, (start, end, kind) in enumerate(tokens[:-1]):
        if kind == "FMID" and data[end:tokens[i + 1][0]] in (b"{", b"}"):
            tokens[i] = (start, end + 1, kind)
    return merge_text(tokens)


def library_files():
    root = sysconfig.get_paths()["stdlib"]
    for directory, subdirectories, files in os.walk(root):
        subdirectories[:] = sorted(d for d in subdirectories if d not in ("site-packages", "__pycache__"))
        yield from (os.path.join(directory, f) for f in sorted(files) if f.endswith(".py"))


def main():
    if sys.version_info < (3, 12):
        print("python_tokenize: skipped — Python 3.12 or later tokenizes f-strings into their parts")
        return 0
    cli = sys.argv[1]
    paths = sys.argv[2:] or list(library_files())
    references = {}
    for path in paths:
        with open(path, "rb") as handle:
            data = handle.read()
        try:
            references[path] = (data, python_tokens(data))
        except (SyntaxError, UnicodeDecodeError, tokenize.TokenError):
            pass  # not Python the reference tokenizer reads: nothing to compare
    failed = False
    for grammar, floor in FLOORS.items():
        compared = identical = 0
        residue = []
        # One CLI process per file, run side by side (each builds its lexer: the cost is in the processes).
        with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
            results = pool.map(lambda item: (item[0], item[1][1], scilex_tokens(cli, grammar, item[0], item[1][0])),
                               references.items())
        for path, expected, (got, error) in results:
            compared += 1
            if got == expected and error is None:
                identical += 1
                continue
            first = next((i for i, (a, b) in enumerate(zip(got, expected)) if a != b), min(len(got), len(expected)))
            where = expected[first] if first < len(expected) else got[first] if first < len(got) else None
            residue.append((os.path.relpath(path, sysconfig.get_paths()["stdlib"]), error or f"first difference {where}"))
        share = identical / compared if compared else 0.0
        verdict = "ok" if share >= floor else "FAIL"
        failed = failed or share < floor
        print(f"python_tokenize: {grammar}: {identical}/{compared} files identical to tokenize "
              f"({share:.2%}, floor {floor:.1%}) — {verdict}")
        for path, why in residue:
            print(f"    {path}: {why}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
