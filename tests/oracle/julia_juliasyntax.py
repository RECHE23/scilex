"""The julia example grammar against the tokenizer of the parser Julia ships (Base.JuliaSyntax), over the
Julia sources of Julia's own standard library and test suite: in every file Julia lexes without an error
token, each byte must fall in the same class — space (blanks, line breaks, comments), string, character,
number, name, operator — and each name and number must start at the same byte, or the run fails below its
floor.

Usage: julia_juliasyntax.py <scilex CLI> [file …]
JULIA names the interpreter (default `julia`; a command line such as `docker run … julia` works when the files
are at the same paths inside). The two tokenizers cut some things differently and both cuts are right — a
string into its delimiters and text, `$x` into `$` and `x`, `.^` into `.` and `^`, a symbol `:s` or a macro
`@m` into its sigil and its name — so bytes are compared by class, the sigils as operators.
"""
import concurrent.futures
import os
import re
import shlex
import subprocess
import sys

from python_tokenize import unescape

# The share of files Julia lexes cleanly that must lex identically.
FLOOR = 1.0
JULIA = shlex.split(os.environ.get("JULIA", "julia"))
DUMP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "julia_tokens.jl")
SPACE = {"Whitespace", "NewlineWs", "Comment"}
STRING = {'"', '"""', "String", "`", "```", "CmdString"}
NUMBER = {"Integer", "BinInt", "HexInt", "OctInt", "Float", "Float32"}
NAME = {"Identifier", "MacroName", "StringMacroName", "CmdMacroName"}
SIGILED = {"MACRO": 1, "SYMBOL": 1, "INTERP_NAME": 1}  # one operator byte, then a name
OURS = {"KEYWORD": "name", "IDENT": "name", "NUMBER": "number", "CHAR": "char", "STRING_OPEN": "string",
        "STRING_TEXT": "string", "STRING_ESCAPE": "string", "STRING_CLOSE": "string", "NEWLINE": "space"}


def julia_dump(paths):
    """{path: [(kind, start byte, end byte)]} for \\p paths, from one Julia process."""
    run = subprocess.run(JULIA + [DUMP] + paths, capture_output=True, check=True)
    tokens = {}
    current = None
    for line in run.stdout.decode("utf-8", "replace").splitlines():
        fields = line.split("\t")
        if fields[0] == "FILE":
            current = tokens.setdefault(fields[1], [])
        else:
            current.append((fields[0], int(fields[1]) - 1, int(fields[2])))
    return tokens


def julia_classes(data, tokens):
    """The class of each byte and the starts of names and numbers, or None when Julia meets an error."""
    classes = ["space"] * len(data)
    starts = set()
    # Where each token stands: in code, or in a string or command (whose indentation Julia's tokenizer gives
    # as whitespace, for the parser to strip). An interpolation's `$(` … `)` is code again.
    context = ["code"]
    depth = []
    for index, (kind, start, end) in enumerate(tokens):
        if kind in ('"', '"""', "`", "```"):
            if context[-1] == kind:
                context.pop()
            elif context[-1] in ("code", "interpolation"):
                context.append(kind)
        elif kind == "(" and context[-1] in ('"', '"""', "`", "```") and index > 0 and tokens[index - 1][0] == "$":
            context.append("interpolation")
            depth.append(0)
        elif kind == "(" and context[-1] == "interpolation":
            depth[-1] += 1
        elif kind == ")" and context[-1] == "interpolation":
            if depth[-1] == 0:
                context.pop()
                depth.pop()
                classes[start:end] = ["operator"] * (end - start)
                continue
            depth[-1] -= 1
        if end <= start:
            continue
        if kind in SPACE:
            if kind == "Whitespace" and context[-1] in ('"', '"""', "`", "```"):
                classes[start:end] = ["string"] * (end - start)
            continue
        if kind.startswith("Error"):
            return None
        if kind in NUMBER and data[start:start + 1] in (b"-", b"+"):
            # Julia's tokenizer takes a prefix sign into a literal (`f(-1)`) where its parser wants it: the
            # sign is compared as the operator it is.
            classes[start] = "operator"
            start += 1
        if kind in STRING:
            label = "string"
        elif kind == "Char" or (kind == "'" and any(tokens[j][0] == "Char" for j in (index - 1, index + 1)
                                                    if 0 <= j < len(tokens))):
            label = "char"
        elif kind in NUMBER:
            label = "number"
        elif kind in NAME or re.fullmatch(r"[a-z_]+", kind):  # a keyword's kind is its word
            label = "name"
        else:
            label = "operator"
        classes[start:end] = [label] * (end - start)
        if label in ("name", "number"):
            starts.add(start)
    return classes, starts


def scilex_classes(cli, path, data):
    """The same from the CLI's tokens, and its error line if it stopped."""
    run = subprocess.run([cli, "--example", "julia", path], capture_output=True)
    line_starts = [0] + [i + 1 for i, byte in enumerate(data) if byte == 10]
    classes = ["space"] * len(data)
    starts = set()
    for line in run.stdout.decode("utf-8", "surrogateescape").split("\n"):
        fields = line.split("\t")
        if len(fields) != 3 or line.startswith("#"):
            continue
        kind, lexeme, position = fields
        row, column = (int(part) for part in position.split(":"))
        start = line_starts[row - 1] + column - 1
        end = start + len(unescape(lexeme))
        if kind in ("STRING_MACRO", "VAR_OPEN"):  # a name, then the quotes that open its literal
            text = unescape(lexeme)
            quotes = len(text) - len(text.rstrip(b'"`'))
            classes[start:end - quotes] = ["name"] * (end - quotes - start)
            classes[end - quotes:end] = ["string"] * quotes
            starts.add(start)
        elif kind in SIGILED:
            classes[start] = "operator"
            classes[start + 1:end] = ["name"] * (end - start - 1)
            starts.add(start + 1)
        elif kind == "INTERP_OPEN":
            classes[start:end] = ["operator"] * (end - start)
        else:
            label = OURS.get(kind, "operator")
            classes[start:end] = [label] * (end - start)
            if label in ("name", "number"):
                starts.add(start)
    return classes, starts, run.stderr.decode("utf-8", "replace").strip()


def compare(cli, path, tokens):
    """A failure line or None, and whether Julia lexed the file without an error token."""
    with open(path, "rb") as source:
        data = source.read()
    want = julia_classes(data, tokens)
    if want is None:
        return None, False
    classes, starts, error = scilex_classes(cli, path, data)
    for offset, (expected, actual) in enumerate(zip(want[0], classes)):
        if expected != actual:
            line = data.count(b"\n", 0, offset) + 1
            context = data[max(0, offset - 12):offset + 12].decode("utf-8", "replace")
            return (f"{path}:{line}: byte {offset} is {actual} for scilex, {expected} for Julia: {context!r}"
                    + (f" ({error})" if error else "")), True
    missing = sorted(want[1] ^ starts)
    if missing:
        line = data.count(b"\n", 0, missing[0]) + 1
        context = data[max(0, missing[0] - 12):missing[0] + 12].decode("utf-8", "replace")
        return f"{path}:{line}: a name or number starts at byte {missing[0]} for one side only: {context!r}", True
    return None, True


def main():
    cli = sys.argv[1]
    files = sys.argv[2:]
    if not files:
        root = subprocess.run(JULIA + ["-e", 'print(joinpath(Sys.BINDIR, "..", "share", "julia"))'],
                              capture_output=True, text=True, check=True).stdout
        files = sorted(os.path.join(directory, name) for sub in ("stdlib", "test")
                       for directory, _, names in os.walk(os.path.join(root, sub)) for name in names
                       if name.endswith(".jl"))
    chunks = [files[i:i + 100] for i in range(0, len(files), 100)]
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
        dumped = {}
        for part in pool.map(julia_dump, chunks):
            dumped.update(part)
        results = list(pool.map(lambda path: compare(cli, path, dumped.get(path, [])), files))
    clean = sum(1 for _, lexed in results if lexed)
    failures = [failure for failure, _ in results if failure]
    for failure in failures[:20]:
        print(failure)
    share = 1 - len(failures) / max(1, clean)
    print(f"julia: {clean - len(failures)}/{clean} files lex as Julia's tokenizer reads them "
          f"({len(files) - clean} with an error token in Julia's; {share:.4f}, floor {FLOOR})")
    return 0 if clean and share >= FLOOR else 1


if __name__ == "__main__":
    sys.exit(main())
