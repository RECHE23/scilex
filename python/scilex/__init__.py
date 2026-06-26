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

Rules can opt into *modes* (contextual lexing): a rule may be limited to named
modes via ``in_mode`` and drive a per-scan mode stack via ``action`` (``("push",
mode)`` / ``("set", mode)`` / ``("pop",)``), so the same byte lexes differently by
context — Python f-strings are the canonical case::

    OPEN, TEXT, LB, NAME, RB, CLOSE = range(6)
    fstr = scilex.Lexer([
        (NAME, r"[A-Za-z_]\w*", False, ["default", "interp"]),  # code, shared
        (OPEN, r'f"', False, ["default", "interp"], ("push", "fstr")),
        (TEXT, r'[^{}"]+', False, ["fstr"]),
        (LB, r"\{", False, ["fstr"], ("push", "interp")),
        (CLOSE, r'"', False, ["fstr"], ("pop",)),
        (RB, r"\}", False, ["interp"], ("pop",)),
    ])
    fstr.tokenize(r'f"hi {name}"')   # OPEN, TEXT, LB, NAME, RB, CLOSE

A plain ``(kind, pattern, skip)`` rule uses neither field. Earlier rules win ties;
the longest match wins overall (maximal munch). Positions
are byte-based (SciLex's UTF-8 model): ``offset`` is a 0-based byte index,
``line``/``column`` are 1-based (a column counts bytes). A position matched by no
rule raises :class:`scilex.error` (aka :class:`LexerError`) carrying the failure
``.position``.
"""
import os

from scilex._scilex import (
    compile as _compile,
    dfa_modes_active as _dfa_modes_active,
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

__version__ = "2026.6.8"

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
    """A lexical token: ``kind`` (int), ``lexeme`` (str), ``position`` (:class:`Position`)
    and ``mode`` (the name of the mode it was lexed in, ``"default"`` at the root).

    ``offset``, ``line`` and ``column`` are exposed directly as read-only shortcuts
    for ``position.offset`` / ``.line`` / ``.column``.
    """

    __slots__ = ("kind", "lexeme", "position", "mode")

    def __init__(self, kind, lexeme, position, mode="default"):
        self.kind = kind
        self.lexeme = lexeme
        self.position = position
        self.mode = mode

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
                f"line={self.position.line}, column={self.position.column}, mode={self.mode!r})")

    def __eq__(self, other):
        if not isinstance(other, Token):
            return NotImplemented
        return (self.kind == other.kind and self.lexeme == other.lexeme
                and self.position == other.position and self.mode == other.mode)

    def __hash__(self):
        return hash((self.kind, self.lexeme, self.position, self.mode))


def _token(fields):
    """Build a :class:`Token` from a (kind, lexeme, offset, line, column, mode) tuple."""
    kind, lexeme, offset, line, column, mode = fields
    return Token(kind, lexeme, Position(offset, line, column), mode)


def _attach_position(exc, source=None):
    """Enrich a lexing :class:`error` with a structured ``.position`` and, when the
    source is known, a ``.context`` snippet around the offending byte.

    Always sets ``.position`` (from the byte fields the C++ layer attaches). When
    ``source`` is given (the tokenize/scan paths have it), also sets ``.context`` — a
    few bytes either side of the offending byte, that byte fenced in ``‹ ›`` — and
    rewrites the message to include the position and that snippet. Layout errors have
    no source here, so they keep ``.position`` only. Positions are **byte** offsets
    (SciLex's UTF-8 model), so the snippet is sliced from the encoded bytes and decoded
    with ``errors="replace"``: a window edge splitting a codepoint shows ``�``, never
    raises.
    """
    offset = getattr(exc, "offset", None)
    if offset is None:
        return
    exc.position = Position(offset, exc.line, exc.column)
    if source is None:
        return
    data = source if isinstance(source, bytes) else source.encode("utf-8")
    window = 8
    before = data[max(0, offset - window):offset].decode("utf-8", "replace")
    here = data[offset:offset + 1].decode("utf-8", "replace")
    after = data[offset + 1:offset + 1 + window].decode("utf-8", "replace")
    exc.context = f"{before}‹{here}›{after}"
    exc.args = (f"no rule matches at line {exc.line}, column {exc.column}: {exc.context}",)


def _normalize_action(action, index):
    """Validate a rule action into ``None`` | ``("push"|"set", mode)`` | ``("pop",)``."""
    if action is None:
        return None
    if not isinstance(action, (tuple, list)) or not action:
        raise TypeError(
            f"rule {index}: action must be None or a tuple "
            "(\"push\", mode) / (\"set\", mode) / (\"pop\",)")
    op = action[0]
    if op in ("push", "set"):
        if len(action) != 2 or not isinstance(action[1], str):
            raise TypeError(f"rule {index}: {op!r} action needs a target mode: ({op!r}, mode)")
        return (op, action[1])
    if op == "pop":
        return ("pop",)
    raise ValueError(f"rule {index}: unknown action {op!r} (expected 'push', 'pop' or 'set')")


class Lexer:
    """A compiled, reusable set of token rules.

    Args:
        rules (iterable): Items ``(kind, pattern[, skip[, in_mode[, action]]])``;
            ``kind`` is an int, ``pattern`` a REAL regex string, ``skip`` a bool
            (default ``False``) — when true, matches are consumed but not emitted.
            ``in_mode`` is a sequence of mode names the rule is active in (empty, the
            default, means the default mode only); ``action`` drives the per-scan mode
            stack and is ``None`` or one of ``("push", mode)`` / ``("set", mode)`` /
            ``("pop",)``. These last two are SciLex's *modes* (contextual lexing); a
            plain ``(kind, pattern, skip)`` rule needs neither.
        insignificant_modes (iterable): Mode names whose tokens carry no layout
            structure (Layout Awareness Level A — see :meth:`layout`): code spanning
            lines in such a mode is treated as continuation. Each must be a mode the
            rules use.
        dfa_modes (iterable): Mode names to accelerate with a DFA fast path (one DFA
            pass replaces the per-rule dispatch). Best-effort and invisible: a mode
            whose rules need an assertion no DFA can represent, or whose DFA fails the
            build-time audit (a lazy quantifier), silently stays on the regular engine
            — see :attr:`dfa_modes_active`. The token stream is identical either way.

    Raises:
        error: If a pattern is an invalid regex, a transition targets an empty mode, or
            ``dfa_modes`` names an unknown mode.
        ValueError: If ``insignificant_modes`` names a mode the rules do not use.
    """

    def __init__(self, rules, insignificant_modes=(), dfa_modes=()):
        normalized = []
        for entry in rules:
            kind, pattern = entry[0], entry[1]
            skip = bool(entry[2]) if len(entry) > 2 else False
            if not isinstance(pattern, str):
                raise TypeError(
                    f"rule {len(normalized)}: pattern must be str, got {type(pattern).__name__}")
            in_mode = entry[3] if len(entry) > 3 and entry[3] is not None else ()
            if isinstance(in_mode, str):
                raise TypeError(
                    f"rule {len(normalized)}: in_mode must be a list of mode names, not a bare str")
            modes = []
            for mode in in_mode:
                if not isinstance(mode, str):
                    raise TypeError(
                        f"rule {len(normalized)}: in_mode entries must be str, "
                        f"got {type(mode).__name__}")
                modes.append(mode)
            action = _normalize_action(entry[4] if len(entry) > 4 else None, len(normalized))
            if modes or action is not None:
                normalized.append((int(kind), pattern, skip, modes, action))
            else:
                normalized.append((int(kind), pattern, skip)) # plain rule stays a triple
        self._rules = normalized
        self._dfa_modes = [str(mode) for mode in dfa_modes]
        # The C++ ctor validates dfa_modes against the interned modes (raising error on
        # an unknown one) and builds the per-mode DFA fast path.
        self._handle = _compile(normalized, self._dfa_modes)
        known = {"default"}
        for entry in normalized:
            if len(entry) > 3:
                known.update(entry[3]) # the rule's in_mode names
        self._insignificant = []
        for mode in insignificant_modes:
            if mode not in known:
                raise ValueError(f"insignificant_modes: unknown mode {mode!r}")
            self._insignificant.append(mode)

    @property
    def rules(self):
        """The normalized rules: ``(kind, pattern, skip)``, or
        ``(kind, pattern, skip, in_mode, action)`` for rules that use modes."""
        return list(self._rules)

    @property
    def insignificant_modes(self):
        """The mode names whose tokens carry no layout structure (Level A)."""
        return list(self._insignificant)

    @property
    def dfa_modes(self):
        """The mode names requested for DFA acceleration (see :attr:`dfa_modes_active`)."""
        return list(self._dfa_modes)

    @property
    def dfa_modes_active(self):
        """The modes actually accelerated by a DFA fast path.

        A mode requested in ``dfa_modes`` but rejected — its rules need an assertion no
        DFA can represent, or its DFA failed the build-time audit (a lazy quantifier) —
        is absent: it fell back to the regular engine, lexing the same tokens. So the
        rejected set is ``set(dfa_modes) - set(dfa_modes_active)``.
        """
        return list(_dfa_modes_active(self._handle))

    def layout(self, tokens):
        """Insert NEWLINE / INDENT / DEDENT from indentation, mode-aware.

        Uses this lexer's :attr:`insignificant_modes` (Layout Awareness Level A):
        a token in an insignificant mode is passed through without shaping
        indentation, so multi-line brackets / flow collections read as continuation.

        Args:
            tokens (iterable[Token]): An end-of-input-terminated token stream
                (tokenize with ``eof=True``).

        Returns:
            list[Token]: The layout-aware tokens (still END_OF_INPUT-terminated).

        Raises:
            error: On a line that dedents to an unknown indentation (``.position``).
        """
        return Layout(self._insignificant).apply(tokens)

    def tokenize(self, text, eof=False):
        """Tokenize ``text`` eagerly into a list.

        Args:
            text (str | bytes): The source to tokenize; each :class:`Token`'s lexeme
                is a ``str`` when ``text`` is ``str``, ``bytes`` when it is ``bytes``.
            eof (bool): Append a terminal :data:`END_OF_INPUT` token at the end.

        Returns:
            list[Token]: Emitted tokens in source order (skip matches omitted).

        Raises:
            error: If some position is matched by no rule (with ``.position``).
        """
        try:
            raw = _tokenize(self._handle, text, bool(eof))
        except error as exc:
            _attach_position(exc, text)
            raise
        return [_token(fields) for fields in raw]

    def scan(self, text, eof=False):
        """Lazily scan ``text``, yielding one :class:`Token` at a time.

        Unlike :meth:`tokenize`, nothing but the current token is held — the
        parser-friendly access pattern. The returned generator runs the lexer on
        demand; a lexical error surfaces while iterating, only after every token
        before it has been yielded.

        ``scan`` holds the GIL for each one-token step (the parser-friendly access
        pattern); for multi-threaded throughput use :meth:`tokenize`, which releases
        the GIL around the scan of inputs of 4 KB or more.

        Args:
            text (str | bytes): The source to scan; each :class:`Token`'s lexeme is a
                ``str`` when ``text`` is ``str``, ``bytes`` when it is ``bytes``.
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
                _attach_position(exc, text)
                raise
            while True:
                try:
                    fields = _scan_next(cursor)
                except error as exc:
                    _attach_position(exc, text)
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

    With ``insignificant_modes`` (Layout Awareness Level A), a token whose
    :attr:`Token.mode` is listed is passed through without shaping indentation, so a
    multi-line bracket or flow collection reads as line continuation. The default
    (none) is the positional pass. :meth:`Lexer.layout` wires a lexer's own modes.

    The reserved kinds are exposed as :attr:`newline_kind`, :attr:`indent_kind` and
    :attr:`dedent_kind`.
    """

    newline_kind = NEWLINE
    indent_kind = INDENT
    dedent_kind = DEDENT

    def __init__(self, insignificant_modes=()):
        self._insignificant = [str(mode) for mode in insignificant_modes]

    def apply(self, tokens):
        """Rewrite ``tokens`` with NEWLINE/INDENT/DEDENT inserted.

        Args:
            tokens (iterable[Token]): An **end-of-input-terminated** token stream —
                tokenize with ``eof=True`` (the terminal :data:`END_OF_INPUT` is
                preserved). Each token's ``kind``, position and ``mode`` are read.

        Returns:
            list[Token]: The layout-aware tokens (still END_OF_INPUT-terminated).

        Raises:
            error: On a line that dedents to an indentation no open block used,
                carrying ``.position`` (but no ``.context`` snippet: ``apply`` receives
                tokens, not the source text, so there is nothing to slice).
        """
        fields = []
        for t in tokens:
            if not isinstance(t.lexeme, str):
                raise TypeError("layout operates on str tokens; got a bytes lexeme — "
                                "tokenize a str source before applying layout")
            fields.append((t.kind, t.lexeme, t.offset, t.line, t.column, t.mode))
        try:
            raw = _layout(fields, self._insignificant)
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


def layout(tokens, insignificant_modes=()):
    """Insert NEWLINE/INDENT/DEDENT into ``tokens`` (see :meth:`Layout.apply`).

    Args:
        tokens (iterable[Token]): An end-of-input-terminated token stream.
        insignificant_modes (iterable): Mode names that carry no layout structure
            (Layout Awareness Level A).

    Returns:
        list[Token]: The layout-aware tokens.
    """
    return Layout(insignificant_modes).apply(tokens)


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
