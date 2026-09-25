# Get started

SciLex is three things over one engine: a header-only C++20 library, an abi3 Python extension, and
a command-line lexer. All three depend on [REAL](https://reche23.github.io/real-regex/)'s headers
and nothing else.

## C++

The library is header-only; add REAL's `include/` and SciLex's to the include path, or use CMake:

```cmake
include(FetchContent)
FetchContent_Declare(scilex GIT_REPOSITORY https://github.com/RECHE23/scilex GIT_TAG v2026.9.1)
set(SCILEX_FETCH_DEPS ON)   # fetch REAL alongside
FetchContent_MakeAvailable(scilex)
target_link_libraries(app PRIVATE scilex::scilex)
```

With REAL and SciLex installed under one prefix, `find_package(scilex CONFIG REQUIRED)` pulls REAL
in transitively.

```cpp
#include <cstdio>
#include <scilex/scilex.hpp>

int main()
{
  std::vector<scilex::rule> rules {
    {.kind = 0, .pattern = real::regex(R"(\s+)"), .skip = true}, // whitespace, skipped
    {.kind = 1, .pattern = real::regex("if")},                   // keyword: before the identifier
    {.kind = 2, .pattern = real::regex("[a-z_][a-z0-9_]*")},     // identifier
    {.kind = 3, .pattern = real::regex("[0-9]+")},               // number
    {.kind = 4, .pattern = real::regex(R"([-+*/=])")},           // operator
  };
  const scilex::lexer lexer {std::move(rules)};
  for (const scilex::token& tok : lexer.scan("if x + 42")) {
    std::printf("%d %.*s\n", tok.kind, static_cast<int>(tok.lexeme.size()), tok.lexeme.data());
  }
}
```

`if` is listed before the identifier rule: both match two bytes of `if`, and on a tie the earlier
rule wins. `scan` yields one token per step; `tokenize` returns them all in a vector.

## Python

```text
pip install scilex
```

The wheel is `cp311-abi3` (CPython 3.11 and later). A rule is `(kind, pattern)`, optionally followed
by `skip`, the modes it is active in, and a mode action:

```python
import scilex

lx = scilex.Lexer([(0, r"\s+", True), (1, r"[0-9]+"), (2, r"[a-z]+")])
for tok in lx.scan("abc 42"):
    print(tok.kind, tok.lexeme, tok.position)
```

`scilex.get_include()` returns the installed headers' directory, for compiling C++ against the same
version.

## The command line

`make cli` builds `build/bin/scilex`. It lexes with a built-in grammar or with yours:

```console
$ scilex --list                       # the built-in example grammars
$ scilex --example json file.json     # lex a file with one
$ echo 'x = 41 + 1' | scilex my.lex   # or with a .lex grammar of your own
IDENT	x	1:1
OP	=	1:3
NUMBER	41	1:5
OP	+	1:8
NUMBER	1	1:10
```

`--layout` adds the indentation tokens, `--errors=token` recovers from unlexable bytes instead of
stopping, and `--columns=codepoints` or `--columns=utf16` counts columns in those units. The grammar
format is described in {doc}`guide/grammar-format`.
