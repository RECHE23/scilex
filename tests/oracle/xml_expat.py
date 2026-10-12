"""The xml example grammar against expat, the parser Python ships, over the XML files of the running
interpreter's test suite: in every file expat parses, each piece of markup — a start or end tag and its
name, a comment, a processing instruction (the XML declaration included), a CDATA section, the document type
— must start where expat reports it, or the run fails below its floor.

Usage: xml_expat.py <scilex CLI> [file …]
Expat reports an empty element's end after its `/>`, a document type at its internal subset, and the markup
an entity reference brings in at the reference: those are matched to the grammar's tokens as such, and the
markup inside a document type is the grammar's one DOCTYPE token.
"""
import concurrent.futures
import glob
import os
import subprocess
import sys
import sysconfig
import xml.parsers.expat

from python_tokenize import unescape

# The share of well-formed files that must lex identically. The grammar reads names as UTF-8: a document in
# another encoding with a non-ASCII name is beyond it.
FLOOR = 1.0
MARKUP = {"TAG<": "start", "CLOSE</": "end", "COMMENT": "comment", "PI": "pi", "CDATA": "cdata",
          "DOCTYPE": "doctype"}


def expat_events(data):
    """Expat's markup events as (kind, byte offset, name), or None when expat refuses the file."""
    parser = xml.parsers.expat.ParserCreate()
    events = []

    def note(kind, name=""):
        events.append((kind, parser.CurrentByteIndex, name))

    parser.StartElementHandler = lambda name, attributes: note("start", name)
    parser.EndElementHandler = lambda name: note("end", name)
    parser.CommentHandler = lambda text: note("comment")
    parser.XmlDeclHandler = lambda version, encoding, standalone: note("pi")
    parser.ProcessingInstructionHandler = lambda target, text: note("pi")
    parser.StartDoctypeDeclHandler = lambda *arguments: note("doctype")
    parser.StartCdataSectionHandler = lambda: note("cdata")
    try:
        parser.Parse(data, True)
    except xml.parsers.expat.ExpatError:
        return None
    return events


def scilex_tokens(cli, path):
    """The CLI's tokens as (kind, start byte, end byte, lexeme bytes), and its error line if it stopped."""
    run = subprocess.run([cli, "--example", "xml", path], capture_output=True)
    with open(path, "rb") as source:
        data = source.read()
    line_starts = [0] + [i + 1 for i, byte in enumerate(data) if byte == 10]
    tokens = []
    for line in run.stdout.decode("utf-8", "surrogateescape").split("\n"):
        fields = line.split("\t")
        if len(fields) != 3 or line.startswith("#"):
            continue
        kind, lexeme, position = fields
        row, column = position.split(":")
        start = line_starts[int(row) - 1] + int(column) - 1
        text = unescape(lexeme)
        tokens.append((kind, start, start + len(text), text))
    return tokens, run.stderr.decode("utf-8", "replace").strip()


def compare(cli, path):
    with open(path, "rb") as source:
        events = expat_events(source.read())
    if events is None:
        return None, False
    tokens, error = scilex_tokens(cli, path)
    doctypes = [(start, end) for kind, start, end, _ in tokens if kind == "DOCTYPE"]
    empty_ends = {end for kind, _, end, _ in tokens if kind == "/>"}
    references = {start for kind, start, _, _ in tokens if kind == "ENTITY"}
    got = []
    for index, (kind, start, _, _) in enumerate(tokens):
        if kind in MARKUP:
            name = ""
            if kind in ("TAG<", "CLOSE</") and index + 1 < len(tokens) and tokens[index + 1][0] == "NAME":
                name = tokens[index + 1][3].decode("utf-8", "replace")
            got.append((MARKUP[kind], start, name))
    want = []
    for kind, offset, name in events:
        inside = next((start for start, end in doctypes if start <= offset < end), None)
        if inside is not None:
            if kind == "doctype":
                want.append(("doctype", inside, ""))
        elif offset in references or (kind == "end" and offset in empty_ends):
            continue
        else:
            want.append((kind, offset, name))
    if want != got:
        for index, (expected, actual) in enumerate(zip(want, got)):
            if expected != actual:
                return f"{path}: markup {index}: expat {expected}, scilex {actual}", True
        return f"{path}: expat {len(want)} pieces of markup, scilex {len(got)}" + (f" ({error})" if error else ""), True
    return None, True


def documents():
    """The XML files of the running interpreter's test suite."""
    tests = os.path.join(sysconfig.get_paths()["stdlib"], "test")
    return sorted(glob.glob(os.path.join(tests, "**", "*.xml"), recursive=True))


def main():
    cli = sys.argv[1]
    files = sys.argv[2:] or documents()
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
        results = list(pool.map(lambda path: compare(cli, path), files))
    checked = sum(1 for _, parsed in results if parsed)
    failures = [failure for failure, _ in results if failure]
    for failure in failures[:20]:
        print(failure)
    share = 1 - len(failures) / max(1, checked)
    print(f"xml: {checked - len(failures)}/{checked} well-formed documents lex as expat reads them "
          f"({len(files) - checked} refused by expat; {share:.4f}, floor {FLOOR})")
    return 0 if checked and share >= FLOOR else 1


if __name__ == "__main__":
    sys.exit(main())
