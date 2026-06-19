r"""SciLex — a generic, linear-time maximal-munch lexer (a thin layer over REAL).

Define an ordered list of rules — ``(kind, pattern, skip)`` — and tokenize text:

    import scilex
    lx = scilex.Lexer([
        (0, r"\s+", True),                       # whitespace, skipped
        (1, r"[0-9]+", False),                   # number
        (2, r"[A-Za-z_][A-Za-z0-9_]*", False),   # identifier
    ])
    for tok in lx.tokenize("foo 42"):
        print(tok.kind, tok.lexeme, tok.position)

Token kinds are plain ints, so an :class:`enum.IntEnum` reads well as the kind set:

    from enum import IntEnum
    class Kind(IntEnum):
        WS = 0; NUMBER = 1; IDENT = 2
    lx = scilex.Lexer([(Kind.WS, r"\s+", True), (Kind.NUMBER, r"[0-9]+", False),
                       (Kind.IDENT, r"[A-Za-z_]\w*", False)])

Two access patterns: :meth:`Lexer.tokenize` returns the whole token list, while
:meth:`Lexer.scan` is a lazy generator yielding one :class:`Token` at a time (the
parser-friendly pattern — no list is built). Pass ``eof=True`` to either to get a
final token of kind :data:`END_OF_INPUT` at the end position.

For indentation-significant languages (Python-like), :class:`Layout` turns an
``eof=True`` token stream into a layout-aware one, inserting :data:`NEWLINE` /
:data:`INDENT` / :data:`DEDENT` tokens from each line's leading column.

Earlier rules win ties; the longest match wins overall (maximal munch). Positions
are byte-based (SciLex's UTF-8 model): ``offset`` is a 0-based byte index,
``line``/``column`` are 1-based (a column counts bytes). A position matched by no
rule raises :class:`scilex.error` (aka :class:`LexerError`) carrying the failure
``.position``.
"""
import os

from scilex._scilex import (
    compile as _compile,
    error,
    layout as _layout,
    scan_next as _scan_next,
    scan_start as _scan_start,
    tokenize as _tokenize,
)

__all__ = [
    "Lexer", "Token", "Position", "Layout", "tokenize", "scan", "layout", "error",
    "LexerError", "END_OF_INPUT", "NEWLINE", "INDENT", "DEDENT", "get_include", "get_config",
]

__version__ = "2026.6.5"

#: Reserved kind of SciLex's synthetic end-of-input token
#: (``std::numeric_limits<int>::min()``); user kinds must avoid it.
END_OF_INPUT = -2147483648

#: Reserved kind: end of a logical line, inserted by :class:`Layout` (INT_MIN + 1).
NEWLINE = -2147483647

#: Reserved kind: indentation increased — the start of a deeper block (INT_MIN + 2).
INDENT = -2147483646

#: Reserved kind: indentation decreased — the end of a block (INT_MIN + 3).
DEDENT = -2147483645

#: Alias of :class:`error`, the exception raised on an invalid pattern or
#: unlexable input. Lexing errors carry a :class:`Position` in ``.position``.
LexerError = error


class Position:
    """A source position: 0-based byte ``offset``, 1-based ``line`` and byte ``column``."""

    __slots__ = ("offset", "line", "column")

    def __init__(self, offset, line, column):
        self.offset = offset
        self.line = line
        self.column = column

    def __repr__(self):
        return f"Position(line={self.line}, column={self.column}, offset={self.offset})"

    def __eq__(self, other):
        if not isinstance(other, Position):
            return NotImplemented
        return (self.offset, self.line, self.column) == (other.offset, other.line, other.column)

    def __hash__(self):
        return hash((self.offset, self.line, self.column))


class Token:
    """A lexical token: ``kind`` (int), ``lexeme`` (str) and ``position`` (:class:`Position`).

    ``offset``, ``line`` and ``column`` are exposed directly as read-only shortcuts
    for ``position.offset`` / ``.line`` / ``.column``.
    """

    __slots__ = ("kind", "lexeme", "position")

    def __init__(self, kind, lexeme, position):
        self.kind = kind
        self.lexeme = lexeme
        self.position = position

    @property
    def offset(self):
        """0-based byte offset of the token's first byte (``position.offset``)."""
        return self.position.offset

    @property
    def line(self):
        """1-based line of the token's first byte (``position.line``)."""
        return self.position.line

    @property
    def column(self):
        """1-based byte column of the token's first byte (``position.column``)."""
        return self.position.column

    def __repr__(self):
        return (f"Token(kind={self.kind}, lexeme={self.lexeme!r}, "
                f"line={self.position.line}, column={self.position.column})")

    def __eq__(self, other):
        if not isinstance(other, Token):
            return NotImplemented
        return (self.kind == other.kind and self.lexeme == other.lexeme
                and self.position == other.position)

    def __hash__(self):
        return hash((self.kind, self.lexeme, self.position))


def _token(fields):
    """Build a :class:`Token` from a (kind, lexeme, offset, line, column) tuple."""
    kind, lexeme, offset, line, column = fields
    return Token(kind, lexeme, Position(offset, line, column))


def _attach_position(exc):
    """Give a lexing :class:`error` a structured ``.position`` from its byte fields."""
    offset = getattr(exc, "offset", None)
    if offset is not None:
        exc.position = Position(offset, exc.line, exc.column)


class Lexer:
    """A compiled, reusable set of token rules.

    Args:
        rules (iterable): Items ``(kind, pattern)`` or ``(kind, pattern, skip)``;
            ``kind`` is an int, ``pattern`` a REAL regex string, ``skip`` a bool
            (default ``False``) — when true, matches are consumed but not emitted.

    Raises:
        error: If a pattern is an invalid regex.
    """

    def __init__(self, rules):
        normalized = []
        for entry in rules:
            kind, pattern = entry[0], entry[1]
            skip = bool(entry[2]) if len(entry) > 2 else False
            normalized.append((int(kind), str(pattern), skip))
        self._rules = normalized
        self._handle = _compile(normalized)

    @property
    def rules(self):
        """The normalized ``(kind, pattern, skip)`` rules."""
        return list(self._rules)

    def tokenize(self, text, eof=False):
        """Tokenize ``text`` eagerly into a list.

        Args:
            text (str): The source to tokenize.
            eof (bool): Append a terminal :data:`END_OF_INPUT` token at the end.

        Returns:
            list[Token]: Emitted tokens in source order (skip matches omitted).

        Raises:
            error: If some position is matched by no rule (with ``.position``).
        """
        try:
            raw = _tokenize(self._handle, text, bool(eof))
        except error as exc:
            _attach_position(exc)
            raise
        return [_token(fields) for fields in raw]

    def scan(self, text, eof=False):
        """Lazily scan ``text``, yielding one :class:`Token` at a time.

        Unlike :meth:`tokenize`, nothing but the current token is held — the
        parser-friendly access pattern. The returned generator runs the lexer on
        demand; a lexical error surfaces while iterating, only after every token
        before it has been yielded.

        Args:
            text (str): The source to scan.
            eof (bool): Yield a terminal :data:`END_OF_INPUT` token at the end.

        Yields:
            Token: The next token in source order (skip matches omitted).

        Raises:
            error: If some position is matched by no rule (with ``.position``).
        """
        handle = self._handle
        flag = bool(eof)

        def _iterate():
            try:
                cursor = _scan_start(handle, text, flag)
            except error as exc:
                _attach_position(exc)
                raise
            while True:
                try:
                    fields = _scan_next(cursor)
                except error as exc:
                    _attach_position(exc)
                    raise
                if fields is None:
                    return
                yield _token(fields)

        return _iterate()


class Layout:
    """Inserts NEWLINE / INDENT / DEDENT tokens from a token stream's indentation.

    Indentation-significant languages (Python-like, e.g. SciLang) read structure
    from leading whitespace. Working purely from token positions, :meth:`apply`
    rewrites a flat token stream into a layout-aware one: a :data:`NEWLINE` at each
    logical line end, and :data:`INDENT` / :data:`DEDENT` where the leading (byte)
    column of a line's first token changes. Blank and comment-only lines carry no
    token, so they add no structure.

    The reserved kinds are exposed as :attr:`newline_kind`, :attr:`indent_kind` and
    :attr:`dedent_kind`.
    """

    newline_kind = NEWLINE
    indent_kind = INDENT
    dedent_kind = DEDENT

    def apply(self, tokens):
        """Rewrite ``tokens`` with NEWLINE/INDENT/DEDENT inserted.

        Args:
            tokens (iterable[Token]): An **end-of-input-terminated** token stream —
                tokenize with ``eof=True`` (the terminal :data:`END_OF_INPUT` is
                preserved). Only each token's ``kind`` and position are read.

        Returns:
            list[Token]: The layout-aware tokens (still END_OF_INPUT-terminated).

        Raises:
            error: On a line that dedents to an indentation no open block used
                (carrying ``.position``).
        """
        fields = [(t.kind, t.lexeme, t.offset, t.line, t.column) for t in tokens]
        try:
            raw = _layout(fields)
        except error as exc:
            _attach_position(exc)
            raise
        return [_token(item) for item in raw]


def tokenize(rules, text, eof=False):
    """Compile ``rules`` and tokenize ``text`` eagerly in one call.

    Args:
        rules (iterable): See :class:`Lexer`.
        text (str): The source to tokenize.
        eof (bool): Append a terminal :data:`END_OF_INPUT` token.

    Returns:
        list[Token]: Emitted tokens in source order.
    """
    return Lexer(rules).tokenize(text, eof=eof)


def scan(rules, text, eof=False):
    """Compile ``rules`` and lazily scan ``text`` in one call.

    Args:
        rules (iterable): See :class:`Lexer`.
        text (str): The source to scan.
        eof (bool): Yield a terminal :data:`END_OF_INPUT` token.

    Yields:
        Token: The next token in source order.
    """
    return Lexer(rules).scan(text, eof=eof)


def layout(tokens):
    """Insert NEWLINE/INDENT/DEDENT into ``tokens`` (see :meth:`Layout.apply`).

    Args:
        tokens (iterable[Token]): An end-of-input-terminated token stream.

    Returns:
        list[Token]: The layout-aware tokens.
    """
    return Layout().apply(tokens)


def get_include():
    """Return the directory to add to a C++ include path for SciLex's headers.

    ``#include <scilex/scilex.hpp>`` resolves against this directory. SciLex is
    header-only and shipped inside the installed package, so a project can
    compile against it located through its Python install. Note that SciLex's
    headers include REAL's, so add ``real.get_include()`` as well.

    Returns:
        str: Absolute path to SciLex's include directory.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    packaged = os.path.join(here, "include")
    if os.path.isdir(os.path.join(packaged, "scilex")):
        return packaged
    return os.path.normpath(os.path.join(here, os.pardir, os.pardir, "include"))


def get_config():
    """Return metadata for embedding the C++ library.

    Returns:
        dict: ``version`` (str), ``include`` (str, see :func:`get_include`), and
        ``cxx_standard`` (str).
    """
    return {
        "version": __version__,
        "include": get_include(),
        "cxx_standard": "c++20",
    }
