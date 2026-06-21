# Type stubs for the SciLex public API (PEP 561; the marker is py.typed).
from collections.abc import Iterable, Iterator

__version__: str

#: Reserved token kinds (see the module docstring).
END_OF_INPUT: int
NEWLINE: int
INDENT: int
DEDENT: int

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

class Position:
    offset: int
    line: int
    column: int
    def __init__(self, offset: int, line: int, column: int) -> None: ...
    def __repr__(self) -> str: ...
    def __eq__(self, other: object) -> bool: ...
    def __hash__(self) -> int: ...

class Token:
    kind: int
    lexeme: str | bytes
    position: Position
    def __init__(self, kind: int, lexeme: str | bytes, position: Position) -> None: ...
    @property
    def offset(self) -> int: ...
    @property
    def line(self) -> int: ...
    @property
    def column(self) -> int: ...
    def __repr__(self) -> str: ...
    def __eq__(self, other: object) -> bool: ...
    def __hash__(self) -> int: ...

class Lexer:
    def __init__(self, rules: Iterable[_Rule]) -> None: ...
    @property
    def rules(self) -> list[_NormRule]: ...
    def tokenize(self, text: str | bytes, eof: bool = ...) -> list[Token]: ...
    def scan(self, text: str | bytes, eof: bool = ...) -> Iterator[Token]: ...

class Layout:
    newline_kind: int
    indent_kind: int
    dedent_kind: int
    def apply(self, tokens: Iterable[Token]) -> list[Token]: ...

def tokenize(rules: Iterable[_Rule], text: str | bytes, eof: bool = ...) -> list[Token]: ...
def scan(rules: Iterable[_Rule], text: str | bytes, eof: bool = ...) -> Iterator[Token]: ...
def layout(tokens: Iterable[Token]) -> list[Token]: ...
def get_include() -> str: ...
def get_config() -> dict[str, str]: ...
