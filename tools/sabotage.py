#!/usr/bin/env python3
"""Break one exact thing, run one exact check, and put it back — with the guards that cost a day.

WHY THIS EXISTS AS A FILE. A green proves nothing until the guard has been seen to go red on the
property it claims to hold, so every substantive change here comes with a sabotage. That sabotage
was rewritten from scratch in each session, and it reacquired the same three defects each time.
Each one produced a wrong conclusion before being noticed:

  1. NO REBUILD AFTER REVERT. The source went back, the built artifact did not, so the next check
     ran against the sabotage. That was diagnosed as an engine bug in code that was already
     correct, and only a hand comparison found the truth.
  2. REBUILD ONLY FOR .cpp. Sabotaging a header left the extension module untouched, so every
     verdict measured unmodified code and every sabotage came back GREEN -- the failure mode a
     sabotage exists to detect, in the tool meant to detect it.
  3. NO UNIQUENESS CHECK ON THE REPLACEMENT. The revert is anchored on the replacement text; when
     that text already occurred elsewhere, the revert refused and left the tree sabotaged.
  4. FINALLY DOES NOT RUN ON SIGTERM. The revert lives in `finally`, which covers a crashing check
     (an exception) and not a killed run (a signal). SIGTERM ends the process without unwinding,
     so a harness timeout that used to fire between apply and revert left the file sabotaged — and
     the next gate would have judged the sabotage, not the code. SIGINT and SIGTERM now raise so
     that `finally` runs. SIGKILL is uncatchable and is not claimed.

WHAT A GREEN MEANS HERE. Not "the code is fine": it means the sabotage did not reach the property,
which is a statement about the SABOTAGE first. Three times in one session a green turned out to be
a witness that never reached the site it claimed to pin -- a pattern stopped by an earlier check, a
count that stayed equal because one divergence became another, a rebuild that never happened. Read
a green as "find a better witness", not as "no guard needed".

WHAT IT WILL NOT DO. It refuses to run on a dirty tree for the file it is about to edit: a revert
restores what this script wrote, not what was already there unsaved, and `git stash` is not a way
out (a stash that saved nothing followed by a pop dequeues ANOTHER session's work -- that happened,
on 117 lines across three engine headers).

USAGE
  tools/sabotage.py --file <path> --label <words> --old <text> --new <text> -- <check...>
  tools/sabotage.py --self-test

  <text> may be multi-line; pass it as one shell argument. The check command runs from the repo
  root; its exit status is the verdict. The ARTIFACT a file feeds is rebuilt before the check and
  again after the revert -- see ARTIFACTS below: a header or the binding's source rebuilds the abi3
  extension module (the library is header-only, so a .hpp edit is a code edit); the C++ checks
  (`make test`, `make fuzz-check`, ...) rebuild what they run themselves. A file with no registered
  artifact says so instead of rebuilding something else. `--self-test` checks the artifact map,
  the rebuild before the check and after the revert, and that SIGTERM restores the file.

EXAMPLE
  tools/sabotage.py --file include/scilex/lexer.hpp --label "end one byte short" \\
      --old 'advance(source, cursor, tok.lexeme.size());' \\
      --new 'advance(source, cursor, tok.lexeme.size() - 1);' \\
      -- make test
"""

from __future__ import annotations

import argparse
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

#: Which artifact a sabotaged file feeds, and the command that rebuilds THAT artifact. Longest
#: prefix wins, so a specific file can override the directory it lives in. A file maps to its own
#: artifact or to none: rebuilding some other artifact for it would read as coverage.
ARTIFACTS: tuple[tuple[str, list[str], str], ...] = (
    ("include/", ["make", "python"], "the abi3 extension module"),
    ("python/src/", ["make", "python"], "the abi3 extension module"),
)


def git(*args: str) -> str:
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True,
                          check=False).stdout


def artifact_for(rel: str) -> tuple[list[str], str] | None:
    """The rebuild command for this file's artifact, or None when this script knows of none.

    None is a NAMED silence, not a fallback: pretending to rebuild by running the recipe for some
    other artifact is worse than saying nothing, because it reads as coverage. A caller then knows
    the verdict rests on the check building whatever it needs.
    """
    if not rel.endswith((".hpp", ".cpp", ".h")):
        return None
    best: tuple[list[str], str] | None = None
    best_len = -1
    for prefix, cmd, label in ARTIFACTS:
        if rel.startswith(prefix) and len(prefix) > best_len:
            best, best_len = (cmd, label), len(prefix)
    return best


def rebuild(cmd: list[str], label: str) -> bool:
    done = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, errors="replace")
    if done.returncode != 0:
        print(f"  sabotage: rebuilding {label} FAILED, so the verdict below would be meaningless:")
        print("\n".join("    " + line for line in done.stderr.strip().split("\n")[-4:]))
        return False
    return True


def _raise_interrupt(signum: int, _frame: object) -> None:
    # KeyboardInterrupt is BaseException: a bare `except Exception` in a check cannot swallow it,
    # and `finally` still runs. The default SIGTERM action does not unwind, which is defect 4.
    raise KeyboardInterrupt(f"sabotage: signal {signum}")


def install_interrupt_handlers() -> None:
    """SIGINT and SIGTERM raise so the revert `finally` runs. SIGKILL is not claimed."""
    for name in ("SIGINT", "SIGTERM"):
        sig = getattr(signal, name, None)
        if sig is not None:
            signal.signal(sig, _raise_interrupt)


CANARY = "tools/sabotage_canary.txt"
CANARY_OLD = "SABOTAGE_CANARY_INTACT"
CANARY_NEW = "SABOTAGE_CANARY_SABOTAGED"


#: The artifact the rebuild phase watches, and the text that proves which source it was compiled
#: from: the message a pop at the root raises, read back through the extension module.
PROBE_SOURCE = "include/scilex/lexer.hpp"
PROBE_REAL = "cannot pop the mode stack: already at the root mode"
PROBE_SABOTAGED = "SABOTAGE PROBE: pop at the root"
PROBE_SCRIPT = ("import sys; sys.path.insert(0, 'python'); import scilex\n"
                "try:\n"
                "    scilex.Lexer([(1, r'\\)', False, [], ('pop',))]).tokenize(')')\n"
                "except scilex.error as exc:\n"
                "    print(exc)\n")


def self_test_artifact_map() -> int:
    """The mapping itself, in milliseconds: the right artifact per file, and no fallback."""
    for rel, wanted in (("include/scilex/lexer.hpp", ["make", "python"]),
                        ("python/src/_scilex.cpp", ["make", "python"])):
        got = artifact_for(rel)
        if got is None or got[0] != wanted:
            print(f"sabotage --self-test: {rel} maps to {got}, expected {wanted}: an artifact that "
                  f"follows the wrong file leaves the check measuring unmodified code.")
            return 2
    for rel in ("tests/test_lexer.cpp", "fuzz/fuzz_lexer.cpp", "README.md"):
        if artifact_for(rel) is not None:
            print(f"sabotage --self-test: {rel} claims an artifact it does not have. A decorative "
                  f"rebuild reads as coverage; a named silence does not.")
            return 2
    print("sabotage --self-test: artifact map OK — 2 files map to their own artifact, 3 to none.")
    return 0


def _probe_message() -> str:
    """The pop-at-root message the built extension module raises."""
    done = subprocess.run([sys.executable, "-c", PROBE_SCRIPT], cwd=ROOT, capture_output=True,
                          text=True, errors="replace")
    return (done.stdout + done.stderr).strip()


def self_test_rebuild() -> int:
    """The artifact before the check AND after the revert: a real run whose check compiles nothing,
    so only this script rebuilding the extension can leave it right at either point.

    The child's own verdict is not the measurement: this phase tests the harness, not a guard.
    """
    if git("status", "--porcelain", "--", PROBE_SOURCE).strip():
        print(f"sabotage --self-test: {PROBE_SOURCE} is dirty — commit first, as a real run requires.")
        return 2
    if not rebuild(["make", "python"], "the abi3 extension module"):
        return 2
    if PROBE_REAL not in _probe_message():
        print("sabotage --self-test: the freshly built extension does not raise the expected message, "
              "so the probe cannot read the state it is about to check.")
        return 2
    seen = ROOT / "build" / "st_probe_seen.txt"
    seen.parent.mkdir(exist_ok=True)
    seen.unlink(missing_ok=True)
    probe = ("import pathlib, subprocess, sys;"
             f"r = subprocess.run([sys.executable, '-c', {PROBE_SCRIPT!r}], capture_output=True, text=True);"
             f"pathlib.Path('build/st_probe_seen.txt').write_text('sabotaged' if {PROBE_SABOTAGED!r} in "
             "r.stdout + r.stderr else 'other')")
    done = subprocess.run(
        [sys.executable, str(ROOT / "tools" / "sabotage.py"),
         "--file", PROBE_SOURCE, "--label", "self-test: artifact after revert",
         "--old", PROBE_REAL, "--new", PROBE_SABOTAGED,
         "--", sys.executable, "-c", probe],
        cwd=ROOT, capture_output=True, text=True, errors="replace")
    mid = seen.read_text().strip() if seen.is_file() else "<the check never ran>"
    seen.unlink(missing_ok=True)
    if mid != "sabotaged":
        print(f"sabotage --self-test: during the run the extension raised {mid!r}, not the sabotage — "
              f"the pre-check rebuild did not happen, so a check would measure unmodified code.")
        print("\n".join("    " + line for line in done.stdout.strip().split("\n")[-6:]))
        return 2
    if PROBE_REAL not in _probe_message():
        print("sabotage --self-test: after the revert the extension still raises the sabotaged message: "
              "the tree is clean and the artifact is not.")
        return 2
    print("sabotage --self-test: rebuild OK — the extension carried the sabotage during the run and "
          "the original after the revert.")
    return 0


def self_test() -> int:
    """SIGTERM a run in flight; the canary must come back. SIGKILL is uncatchable and untested."""
    if os.name == "nt":
        print("sabotage --self-test: skipped (no Unix SIGTERM)")
        return 0
    path = ROOT / CANARY
    original = path.read_text()
    if original.count(CANARY_OLD) != 1 or CANARY_NEW in original:
        print("sabotage --self-test: canary is not in the expected state; restore it:")
        print(f"    git checkout -- {CANARY}")
        return 2
    proc = subprocess.Popen(
        [sys.executable, str(ROOT / "tools" / "sabotage.py"),
         "--file", CANARY, "--label", "signal-revert",
         "--old", CANARY_OLD, "--new", CANARY_NEW,
         "--", sys.executable, "-c", "import time; time.sleep(60)"],
        cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    )
    try:
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            if CANARY_NEW in path.read_text():
                break
            if proc.poll() is not None:
                out, _ = proc.communicate()
                print("sabotage --self-test: child exited before applying the sabotage:")
                print(out)
                return 2
            time.sleep(0.02)
        else:
            proc.kill()
            proc.wait(timeout=2)
            print("sabotage --self-test: sabotage never applied")
            return 2
        os.kill(proc.pid, signal.SIGTERM)
        try:
            out, _ = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=2)
            print("sabotage --self-test: did not exit after SIGTERM")
            if path.read_text() != original:
                path.write_text(original)
            return 2
        got = path.read_text()
        if got != original:
            print("sabotage --self-test: file NOT restored after SIGTERM")
            path.write_text(original)
            print(out)
            return 2
        print("sabotage --self-test: SIGTERM restored the canary")
        return 0
    except Exception:
        if path.exists() and path.read_text() != original:
            path.write_text(original)
        raise


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(add_help=True, description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true",
                    help="SIGTERM a run in flight and require the canary file to come back")
    ap.add_argument("--file", help="path, relative to the repo root")
    ap.add_argument("--label", help="what this sabotage claims to break, in a few words")
    ap.add_argument("--old", help="exact text to replace (must occur EXACTLY once)")
    ap.add_argument("--new", help="replacement (must occur ZERO times before)")
    ap.add_argument("check", nargs=argparse.REMAINDER,
                    help="-- followed by the command whose exit status is the verdict")
    args = ap.parse_args(argv)
    if args.self_test:
        # Three phases, all or nothing: the mapping, the artifact after a revert, then the signal.
        for phase in (self_test_artifact_map, self_test_rebuild, self_test):
            code = phase()
            if code != 0:
                return code
        return 0
    if not args.file or not args.label or args.old is None or args.new is None:
        ap.error("--file, --label, --old and --new are required (or pass --self-test)")

    check = [a for a in args.check if a != "--"]
    if not check:
        ap.error("no check command given (put it after --)")

    src = ROOT / args.file
    if not src.is_file():
        print(f"  sabotage: {args.file} is not a file")
        return 2

    rel = str(Path(args.file))
    if git("status", "--porcelain", "--", rel).strip():
        print(f"  sabotage: REFUSING -- {rel} has uncommitted changes.")
        print("  The revert below restores what this script wrote, not what you have unsaved.")
        print("  Commit first. Do NOT reach for `git stash`: a stash that saved nothing followed")
        print("  by a pop dequeues another session's work, which has happened here.")
        return 2

    text = src.read_text()
    if text.count(args.old) != 1:
        print(f"  sabotage: INVALID [{args.label}] -- the anchor occurs "
              f"{text.count(args.old)} times, it must occur exactly once.")
        return 2
    if args.new in text:
        print(f"  sabotage: INVALID [{args.label}] -- the REPLACEMENT already occurs in the file.")
        print("  The revert is anchored on it, so it would refuse and leave the tree sabotaged.")
        return 2

    install_interrupt_handlers()
    src.write_text(text.replace(args.old, args.new, 1))
    verdict = 3
    child: subprocess.Popen[str] | None = None
    try:
        artifact = artifact_for(rel)
        if artifact is not None and not rebuild(*artifact):
            return 2
        if artifact is None:
            print(f"  sabotage: no artifact registered for {rel} -- nothing is prebuilt, so the "
                  f"check below has to build whatever it measures.")
        # errors="replace", not the default strict: a check's output is a BYTE stream, and a test
        # suite that prints a failing pattern prints whatever bytes that pattern holds. A byte-mode
        # witness (`\<C3>`) is not valid UTF-8, so strict decoding raised inside communicate() --
        # AFTER the check had run and BEFORE its exit status was read, which discards the verdict
        # and reports a harness traceback in its place. The one thing this script exists to deliver
        # is that verdict; it must not be lost to the contents of the output.
        child = subprocess.Popen(check, cwd=ROOT, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, text=True, errors="replace")
        out, _ = child.communicate()
        bit = child.returncode != 0
        print(f"  [{args.label}] " + ("RED — the guard reacted." if bit else
                                      "GREEN — NOTHING REACTED."))
        if not bit:
            print("    A green is a statement about the SABOTAGE first: it did not reach the")
            print("    property. Find a witness that does before concluding the guard is missing.")
        for line in out.split("\n"):
            if any(k in line for k in ("checks failed", "FAILED", "AssertionError", "Error ")):
                print(f"    {line.strip()[:110]}")
                break
        verdict = 0 if bit else 1
    except KeyboardInterrupt:
        print("  sabotage: interrupted — reverting")
        if child is not None and child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=2)
            except subprocess.TimeoutExpired:
                child.kill()
        verdict = 2
    finally:
        back = src.read_text()
        if back.count(args.new) == 1:
            src.write_text(back.replace(args.new, args.old, 1))
        else:
            # No `return` here: a return inside finally swallows an exception in flight, and this
            # branch is exactly where one is most likely (the check crashed mid-edit). Print and let
            # the outer status stand -- the message is what the reader needs.
            print(f"  sabotage: CANNOT REVERT -- the replacement now occurs "
                  f"{back.count(args.new)} times in {rel}. Restore it by hand:")
            print(f"    git checkout -- {rel}")
            verdict = 2
        if artifact is not None:
            rebuild(*artifact)  # the artifact must not outlive the sabotage; this is defect 1
        dirty = git("status", "--porcelain", "--", rel).strip()
        state = 'CLEAN' if not dirty else 'STILL DIRTY: ' + dirty
        built = artifact[1] if artifact is not None else "no registered artifact"
        print(f"    reverted ({built} rebuilt) — {rel}: {state}")
    return verdict


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
