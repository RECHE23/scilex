"""Guard against a stale REAL build/install (the gate-hole this closes).

The scilex extension bakes in the REAL version it was compiled against (`scilex.real_version()`,
from real/version.hpp). A build compiled against an OLDER real would silently run old semantics —
e.g. ASCII `\\s`/`\\w` after REAL made them Unicode — which would false-pass the DFA-mode tests
(a Unicode shorthand is not DFA-representable, so those modes would demote silently). So every test
asserts, in setUp, that the compiled-against version is at least the pinned `real-regex>=…` in
pyproject.toml; a stale env fails loudly with "rebuild" rather than passing on old behaviour.
"""
import os
import re

import scilex

_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def _version_tuple(text):
    return tuple(int(part) for part in text.strip().split("."))


def pinned_real_min():
    """The minimum real-regex version pinned in pyproject.toml, as a comparable tuple."""
    with open(os.path.join(_ROOT, "pyproject.toml"), encoding="utf-8") as f:
        match = re.search(r"real-regex>=([0-9.]+)", f.read())
    if match is None:
        raise AssertionError("pyproject.toml has no 'real-regex>=' pin")
    return _version_tuple(match.group(1))


def assert_real_pinned(test_case):
    """Fail the test if scilex was built against a REAL older than the pin (stale build/install)."""
    built = scilex.real_version()
    minimum = pinned_real_min()
    test_case.assertGreaterEqual(
        _version_tuple(built), minimum,
        f"stale REAL: scilex was built against real-regex {built}, below the pinned "
        f">={'.'.join(map(str, minimum))} — rebuild the extension against the pinned real "
        f"(make python) or update the pin.")


# Run the guard at import time too: any test module that imports this fails loudly at collection if
# the compiled-against REAL is stale, not just those that call assert_real_pinned() in setUp.
_built = _version_tuple(scilex.real_version())
if _built < pinned_real_min():
    raise AssertionError(
        f"stale REAL: scilex was built against real-regex {scilex.real_version()}, below the pinned "
        f">={'.'.join(map(str, pinned_real_min()))} — rebuild the extension (make python).")
