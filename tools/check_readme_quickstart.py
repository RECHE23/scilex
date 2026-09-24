#!/usr/bin/env python3
"""The README's C++ block is the tested quickstart: `#include <scilex/scilex.hpp>`, a blank line, then the
region between `// [quickstart]` and `// [/quickstart]` of examples/cpp/quickstart.cpp, dedented. Exit 1,
naming the first differing line, when the README drifts from the program `make example` compiles and runs."""
import pathlib
import sys
import textwrap

ROOT = pathlib.Path(__file__).resolve().parent.parent


def expected_block(source: str) -> str:
    start = source.index("// [quickstart]\n") + len("// [quickstart]\n")
    region = source[start:source.index("// [/quickstart]", start)]
    region = region[: region.rstrip(" ").rfind("\n") + 1]  # drop the end marker's own indentation
    return "#include <scilex/scilex.hpp>\n\n" + textwrap.dedent(region)


def readme_block(readme: str) -> str:
    start = readme.index("## C++ API\n")
    start = readme.index("```cpp\n", start) + len("```cpp\n")
    return readme[start:readme.index("```\n", start)]


def check(source: str, readme: str) -> list[str]:
    want, got = expected_block(source), readme_block(readme)
    if want == got:
        return []
    for n, (w, g) in enumerate(zip(want.splitlines(), got.splitlines()), 1):
        if w != g:
            return [f"README C++ block line {n}: {g!r}, the tested quickstart has {w!r}"]
    return [f"README C++ block has {len(got.splitlines())} lines, the tested quickstart {len(want.splitlines())}"]


def self_test() -> int:
    source = "int main() {\n  // [quickstart]\n  int x {1};\n  // [/quickstart]\n}\n"
    good = "## C++ API\n\n```cpp\n#include <scilex/scilex.hpp>\n\nint x {1};\n```\n"
    cases = [("the same block", good, False),
             ("a changed line", good.replace("{1}", "{2}"), True),
             ("an extra line", good.replace("int x {1};\n", "int x {1};\nx = 3;\n"), True),
             ("a missing include", good.replace("#include <scilex/scilex.hpp>\n\n", ""), True)]
    failures = 0
    for label, readme, should_fail in cases:
        if bool(check(source, readme)) != should_fail:
            print(f"check_readme_quickstart: SELF-TEST FAILED -- {label}")
            failures += 1
    if failures:
        return 1
    print(f"check_readme_quickstart: self-test OK -- {len(cases)} cases, each difference refused alone")
    return 0


def main() -> int:
    if "--self-test" in sys.argv:
        return self_test()
    problems = check((ROOT / "examples/cpp/quickstart.cpp").read_text(encoding="utf-8"),
                     (ROOT / "README.md").read_text(encoding="utf-8"))
    for problem in problems:
        print(f"check_readme_quickstart: {problem}")
    if problems:
        return 1
    print("check_readme_quickstart: OK -- the README shows the quickstart make example compiles and runs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
