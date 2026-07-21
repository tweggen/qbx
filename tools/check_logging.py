#!/usr/bin/env python3
"""Guard the logging boundary (proposal 24).

Every diagnostic in Smaragd goes through the TwLog sink -- the TW_LOG* macros,
the twsyslog.h shim, or Qt's qDebug family (which main.cpp routes into TwLog).
Direct writes to stderr/stdout bypass the ring, the rotating file and the in-app
log dock, and they carry no level, category or thread.

This checker exists because the codebase already drifted that way once: because
qWarning() was invisible on the Windows/MinGW build, plan/STATE.md's standing
advice was "use fprintf(stderr), not qWarning" -- which is how 104 raw call
sites accumulated. main.cpp's message handler removes the reason; this removes
the drift.

Run from the repo root:  python tools/check_logging.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SCAN = [os.path.join("smaragd", "tw303a"), os.path.join("smaragd", "main")]

# Paths where writing to a standard stream IS the program's job, not logging.
ALLOW_FILES = {
    # The TAP output the qxa harness parses, plus argument errors that must
    # reach a caller who has not configured logging yet.
    os.path.join("smaragd", "main", "shell", "src", "main.cpp"),
    # The sink's own console tee has to reach stderr by definition.
    os.path.join("smaragd", "tw303a", "core", "src", "twlog.cc"),
    # A standalone test driver that lives outside a tests/ directory; its
    # PASS/FAIL lines are the program's output, not diagnostics.
    os.path.join("smaragd", "main", "testkit", "src", "action_roundtrip_test.cpp"),
}

# Directory fragments that are exempt wholesale: test drivers print results.
ALLOW_DIRS = (
    os.sep + "tests" + os.sep,
    os.sep + "tools" + os.sep,
)

BAD = re.compile(
    r"""(
          \bfprintf \s* \( \s* std(err|out)\b     # fprintf(stderr, ...)
        | \bfputs   \s* \( .* \b std(err|out) \b  # fputs(..., stderr)
        | \bfputc   \s* \( .* \b std(err|out) \b
        | (?<![\w.>]) printf \s* \(               # bare printf(
        | \bstd::c(out|err) \s* <<                # std::cout << / std::cerr <<
        )""",
    re.VERBOSE,
)

# An inline escape hatch for the rare deliberate case.
ALLOW_COMMENT = "check_logging: allow"


def scan_file(path, rel):
    hits = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for n, line in enumerate(f, 1):
            if ALLOW_COMMENT in line:
                continue
            s = line.lstrip()
            if s.startswith("//") or s.startswith("*"):
                continue
            m = BAD.search(line)
            if m:
                hits.append((n, line.rstrip(), m.group(0).strip()))
    return hits


def main():
    failures = []
    for base in SCAN:
        for dirpath, _dirs, names in os.walk(os.path.join(ROOT, base)):
            if any(d in dirpath + os.sep for d in ALLOW_DIRS):
                continue
            for name in names:
                if not name.endswith((".c", ".cc", ".cpp", ".h", ".hh")):
                    continue
                path = os.path.join(dirpath, name)
                rel = os.path.relpath(path, ROOT)
                if rel in ALLOW_FILES:
                    continue
                for n, line, what in scan_file(path, rel):
                    failures.append((rel, n, line, what))

    if not failures:
        print("check_logging: OK -- no direct stderr/stdout writes")
        return 0

    print("check_logging: %d direct stream write(s) that should go through "
          "TW_LOG* / syslog() instead:\n" % len(failures))
    for rel, n, line, what in failures:
        print("  %s:%d: %s" % (rel.replace(os.sep, "/"), n, line.strip()))
    print("\nUse TW_LOGE/W/I/D(\"<category>\", ...) from tw/core/twlog.h.")
    print("If a call really must write to a stream, append a "
          "'// %s' comment." % ALLOW_COMMENT)
    return 1


if __name__ == "__main__":
    sys.exit(main())
