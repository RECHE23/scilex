# Modes

A flat rule list cannot separate contexts where the same byte means different things: `{` opens an
f-string interpolation but a dict elsewhere, `<` opens a tag in XML content but is a character inside
CDATA. A **mode** is a set of rules active together. A rule names the modes it is active in and may
change the mode when it wins:

- **push** enters a mode, remembering the current one — a nested context;
- **pop** returns to the mode below;
- **set** replaces the current mode, the depth unchanged.

Maximal munch, the tie-break by rule order and the first-byte dispatch run per mode; a rule list
without modes is one mode, `default`.

```pycon
>>> import scilex
>>> NAME, OPEN, TEXT, LB, RB, CLOSE = range(6)
>>> fstr = scilex.Lexer([
...     (NAME, r"[a-z]+", False, ["default", "interp"]),                  # code, in both places
...     (OPEN, r'f"', False, ["default", "interp"], ("push", "fstr")),
...     (TEXT, r'[^{}"]+', False, ["fstr"]),
...     (LB, r"\{", False, ["fstr"], ("push", "interp")),
...     (CLOSE, r'"', False, ["fstr"], ("pop",)),
...     (RB, r"\}", False, ["interp"], ("pop",)),
... ])
>>> [t.kind for t in fstr.tokenize('f"hi {name}"')]
[1, 2, 3, 0, 4, 5]
>>> [t.mode for t in fstr.tokenize('f"hi {name}"')]
['default', 'fstr', 'fstr', 'interp', 'interp', 'fstr']
```

Each token records the mode it was lexed in — the mode *before* its rule's transition — which is
what {doc}`layout` reads to tell structural lines from continuations.

In C++ the same rules set `in_mode` and `action`:

```cpp
using op = scilex::mode_action::op;
scilex::rule open {.kind = OPEN, .pattern = real::regex("f\"")};
open.in_mode = {"default", "interp"};
open.action  = scilex::mode_action {.operation = op::push, .target = "fstr"};
```

## What can go wrong

- **Input ending inside a pushed mode** is an error, `unterminated mode 'fstr' (entered at …)`, which
  names where the mode was entered.
- **A pop at the root** is an error: there is nothing to return to.
- **The stack is bounded** by `scilex::max_mode_depth` (65 536 frames, about 2 MiB): a push past it is
  an error, so an input made only of openers cannot grow the stack without end.

Under {doc}`error recovery <errors>` the first becomes a zero-width error token at the end of input;
the other two stay fatal. The examples `examples/python.hpp` (f-strings, five modes), `examples/xml.hpp`
(content and tag) and `examples/yaml.hpp` (block and flow) are complete modal grammars.
