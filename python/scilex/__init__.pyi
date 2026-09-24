# Type stubs for the SciLex public API (PEP 561; the marker is py.typed).
from collections.abc import Iterable, Iterator
from typing import Final, Literal, final

__all__ = [
    "Lexer", "Token", "Position", "Layout", "tokenize", "scan", "layout", "error",
    "LexerError", "END_OF_INPUT", "NEWLINE", "INDENT", "DEDENT", "ERROR", "get_include", "get_config",
    "real_version",
]

__version__: str

#: How layout measures indentation (see Layout.apply).
_Tabs = Literal["columns", "python"]

#: Reserved token kinds (see the module docstring).
END_OF_INPUT: Final[int]
NEWLINE: Final[int]
INDENT: Final[int]
DEDENT: Final[int]
ERROR: Final[int]

class error(Exception):
    # Attributes attached by the lexing paths (see _attach_position):
    position: Position
    offset: int
    line: int
    column: int
    context: str

LexerError = error

#: A mode transition: ("push", mode), ("set", mode), or ("pop",).
_Action = tuple[str, str] | tuple[str]

#: A rule: (kind, pattern[, skip[, in_mode[, action]]]). in_mode is a sequence of
#: mode names the rule is active in; action drives the mode stack (or None).
_Rule = (
    tuple[int, str]
    | tuple[int, str, bool]
    | tuple[int, str, bool, Iterable[str]]
    | tuple[int, str, bool, Iterable[str], _Action | None]
)

#: A rule as normalized by Lexer.rules: a plain triple, or the modal 5-tuple.
_NormRule = tuple[int, str, bool] | tuple[int, str, bool, list[str], _Action | None]

# Position and Token are extension types that cannot be subclassed.
@final
class Position:
    offset: int
    line: int
    column: int
    def __init__(self, offset: int, line: int, column: int) -> None: ...
    def __repr__(self) -> str: ...
    def __eq__(self, value: object, /) -> bool: ...
    def __hash__(self) -> int: ...

@final
class Token:
    kind: int
    lexeme: str | bytes
    position: Position
    mode: str
    def __init__(self, kind: int, lexeme: str | bytes, position: Position, mode: str = ...) -> None: ...
    @property
    def offset(self) -> int: ...
    @property
    def line(self) -> int: ...
    @property
    def column(self) -> int: ...
    def __repr__(self) -> str: ...
    def __eq__(self, value: object, /) -> bool: ...
    def __hash__(self) -> int: ...

class Lexer:
    def __init__(
        self,
        rules: Iterable[_Rule],
        insignificant_modes: Iterable[str] = ...,
        dfa_modes: Iterable[str] = ...,
        errors: Literal["raise", "token"] = ...,
        columns: Literal["bytes", "codepoints", "utf16"] = ...,
        dfa: Literal["auto", "requested"] = ...,
    ) -> None: ...
    @property
    def column_unit(self) -> Literal["bytes", "codepoints", "utf16"]: ...
    @property
    def rules(self) -> list[_NormRule]: ...
    @property
    def insignificant_modes(self) -> list[str]: ...
    @property
    def dfa_modes(self) -> list[str]: ...
    @property
    def dfa_modes_active(self) -> list[str]: ...
    def tokenize(self, text: str | bytes, eof: bool = ...) -> list[Token]: ...
    def scan(self, text: str | bytes, eof: bool = ...) -> Iterator[Token]: ...
    def layout(self, tokens: Iterable[Token], source: str | None = ..., tabs: _Tabs = ...) -> list[Token]: ...

class Layout:
    newline_kind: int
    indent_kind: int
    dedent_kind: int
    def __init__(self, insignificant_modes: Iterable[str] = ...) -> None: ...
    def apply(self, tokens: Iterable[Token], source: str | None = ..., tabs: _Tabs = ...) -> list[Token]: ...

def tokenize(rules: Iterable[_Rule], text: str | bytes, eof: bool = ...) -> list[Token]: ...
def scan(rules: Iterable[_Rule], text: str | bytes, eof: bool = ...) -> Iterator[Token]: ...
def layout(
    tokens: Iterable[Token], insignificant_modes: Iterable[str] = ..., source: str | None = ..., tabs: _Tabs = ...
) -> list[Token]: ...
def get_include() -> str: ...
def get_config() -> dict[str, str]: ...
def real_version() -> str: ...
