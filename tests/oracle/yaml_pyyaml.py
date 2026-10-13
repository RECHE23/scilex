"""The yaml example grammar against PyYAML's scanner, over the YAML files of the oracle venv (the pinned
documentation toolchain and PyYAML) and this repository's workflows: each file must lex to the same explicit
tokens — where each starts and ends, and a coarse class (plain, double- or single-quoted or block scalar, `:`,
`?`, `-`, `,`, a flow bracket, anchor, alias, tag, document marker, directive) — as `yaml.scan` gives, or the run
fails below its floor. A block scalar (`|`, `>`) and a plain scalar continued on the next lines are tokens the
lexer bounds by the indentation of the node they belong to (Layout Awareness Level B). A file PyYAML refuses is
counted apart, and printed by name.

Usage: yaml_pyyaml.py <scilex CLI> [file …]   (run with the interpreter that has PyYAML: make yaml-oracle)
"""
import concurrent.futures
import glob
import os
import subprocess
import sys
import sysconfig

import yaml

from python_tokenize import unescape

# The share of files within the grammar's reach that must lex identically.
FLOOR = 1.0
OURS = {"SCALAR": "plain", "DQSTRING": "double", "SQSTRING": "single", "BLOCK": "block", ":": "value", "?": "key",
        "-": "entry",
        ",": "flow-entry", "FLOW_OPEN": "open", "FLOW_CLOSE": "close", "ANCHOR": "anchor", "ALIAS": "alias",
        "TAG": "tag", "DOCUMENT": "document", "DIRECTIVE": "directive"}
THEIRS = {"ValueToken": "value", "KeyToken": "key", "BlockEntryToken": "entry", "FlowEntryToken": "flow-entry",
          "FlowSequenceStartToken": "open", "FlowMappingStartToken": "open", "FlowSequenceEndToken": "close",
          "FlowMappingEndToken": "close", "AnchorToken": "anchor", "AliasToken": "alias", "TagToken": "tag",
          "DocumentStartToken": "document", "DocumentEndToken": "document", "DirectiveToken": "directive"}


def pyyaml_tokens(text):
    """PyYAML's explicit tokens as (class, start, end) in characters; an exception's text when PyYAML refuses
    the file."""
    tokens = []
    try:
        for token in yaml.scan(text):
            start, end = token.start_mark.index, token.end_mark.index
            name = type(token).__name__
            if name == "ScalarToken":
                tokens.append(({None: "plain", '"': "double", "'": "single", "|": "block", ">": "block"}[token.style],
                               start, end))
            elif name in THEIRS and end > start:  # an implicit key or value has no characters
                tokens.append((THEIRS[name], start, end))
    except yaml.YAMLError as error:
        return str(error).split("\n")[0]
    return tokens


def scilex_tokens(cli, path, text):
    """The CLI's tokens as (class, start, end) in characters, and its error line if it stopped."""
    run = subprocess.run([cli, "--example", "yaml", "--no-layout", "--columns=codepoints", path], capture_output=True)
    line_starts = [0] + [i + 1 for i, char in enumerate(text) if char == "\n"]
    tokens = []
    for line in run.stdout.decode("utf-8", "surrogateescape").split("\n"):
        fields = line.split("\t")
        if len(fields) != 3 or line.startswith("#"):
            continue
        kind, lexeme, position = fields
        if kind in ("NEWLINE", "INDENT", "DEDENT", "EOF"):  # the layout pass's, compared by its own tests
            continue
        row, column = (int(part) for part in position.split(":"))
        start = line_starts[row - 1] + column - 1
        tokens.append((OURS[kind], start, start + len(unescape(lexeme).decode("utf-8", "replace"))))
    return tokens, run.stderr.decode("utf-8", "replace").strip()


def compare(cli, path):
    """A failure line or None, and whether the file is within the grammar's reach."""
    with open(path, "rb") as source:
        text = source.read().decode("utf-8", "replace")
    want = pyyaml_tokens(text)
    if isinstance(want, str):
        return None, False
    got, error = scilex_tokens(cli, path, text)
    for index, (expected, actual) in enumerate(zip(want, got)):
        if expected != actual:
            line = text.count("\n", 0, expected[1]) + 1
            return f"{path}:{line}: token {index}: PyYAML {expected}, scilex {actual}", True
    if len(want) != len(got):
        return f"{path}: PyYAML {len(want)} tokens, scilex {len(got)}" + (f" ({error})" if error else ""), True
    return None, True


def documents():
    """The YAML files of the running interpreter's installation and of this repository's workflows."""
    roots = [sysconfig.get_paths()["stdlib"], sysconfig.get_paths()["purelib"],
             os.path.join(os.path.dirname(__file__), "..", "..", ".github")]
    return sorted({os.path.normpath(path) for root in roots for pattern in ("*.yml", "*.yaml")
                   for path in glob.glob(os.path.join(root, "**", pattern), recursive=True)})


def main():
    cli = sys.argv[1]
    files = sys.argv[2:] or documents()
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
        results = list(pool.map(lambda path: compare(cli, path), files))
    within = [path for path, (_, reached) in zip(files, results) if reached]
    failures = [failure for failure, _ in results if failure]
    for failure in failures[:20]:
        print(failure)
    for path in sorted(set(files) - set(within)):
        print(f"refused by PyYAML: {path}")
    share = 1 - len(failures) / max(1, len(within))
    print(f"yaml: {len(within) - len(failures)}/{len(within)} files lex as PyYAML scans them "
          f"({len(files) - len(within)} refused by PyYAML; {share:.4f}, floor {FLOOR})")
    return 0 if within and share >= FLOOR else 1


if __name__ == "__main__":
    sys.exit(main())
