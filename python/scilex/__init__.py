r"""SciLex — a generic, linear-time maximal-munch lexer (a thin layer over REAL).

Define an ordered list of rules — ``(kind, pattern, skip)`` — and tokenize text:

    import scilex
    lx = scilex.Lexer([
        (0, r"\s+", True),                       # whitespace, skipped
        (1, r"[0-9]+", False),                   # number
        (2, r"[A-Za-z_][A-Za-z0-9_]*", False),   # identifier
    ])
    for tok in lx.tokenize("foo 42"):
        print(tok.kind, tok.lexeme)

Earlier rules win ties; the longest match wins overall (maximal munch). Positions
are byte-based (SciLex's UTF-8 model): ``offset`` is a 0-based byte index,
``line``/``column`` are 1-based (a column counts bytes). A position matched by no
rule raises :class:`scilex.error`.
"""
import os
from collections import namedtuple

from scilex._scilex import compile as _compile, error, tokenize as _tokenize

__all__ = [
    "Lexer", "Token", "tokenize", "error", "END_OF_INPUT", "get_include", "get_config",
]

__version__ = "2026.6.5"

#: Reserved kind of SciLex's synthetic end-of-input token
#: (``std::numeric_limits<int>::min()``); user kinds must avoid it.
END_OF_INPUT = -2147483648

#: A lexed token: ``kind`` (int), ``lexeme`` (str), ``offset``/``line``/``column`` (int).
Token = namedtuple("Token", ["kind", "lexeme", "offset", "line", "column"])


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

    def tokenize(self, text):
        """Tokenize ``text``.

        Args:
            text (str): The source to tokenize.

        Returns:
            list[Token]: Emitted tokens in source order (skip matches omitted).

        Raises:
            error: If some position is matched by no rule.
        """
        return [Token(*fields) for fields in _tokenize(self._handle, text)]


def tokenize(rules, text):
    """Compile ``rules`` and tokenize ``text`` in one call.

    Args:
        rules (iterable): See :class:`Lexer`.
        text (str): The source to tokenize.

    Returns:
        list[Token]: Emitted tokens in source order.
    """
    return Lexer(rules).tokenize(text)


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
