#!/usr/bin/env python3
"""Guard the <algorithm> include, because libc++ does not hand it out for free.

A file that calls std::max, std::sort or std::fill must include <algorithm>
ITSELF. On libstdc++ it usually gets away without one -- <string>, <vector> and
friends drag <algorithm> in transitively -- so the whole class of omission is
invisible on this repo's regular Linux/Windows boxes and compiles clean for
years. Apple clang's libc++ includes far less transitively, and the same file
then fails with

    error: no member named 'max' in namespace 'std'; did you mean 'fmax'?

which is what happened to body_chain_test.cc, reported from a macOS box (PR
#169). Three headers bite that way; <algorithm> is the one that actually has,
and it is the one this checker enforces.

DELIBERATELY NOT ENFORCED, and the numbers are the reason: measured on the tree
this landed against, 106 files use a std::-qualified <cstdint> spelling without
including <cstdint>, and 120 do the same for <cstddef>, against 14 for
<algorithm>. Those two ride in on <string>/<vector> on every implementation this
project has ever been built with, so a 226-file mechanical change would buy
portability nobody has been able to demonstrate a need for. If a build ever does
fail on one of them, add the rule here rather than fixing the one file.

Run from the repo root:  python tools/check_includes.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SCAN = [os.path.join("smaragd", "tw303a"), os.path.join("smaragd", "main")]

EXTS = (".c", ".cc", ".cpp", ".h", ".hh")

# The <algorithm> names this codebase actually uses. Kept as an explicit list
# rather than a catch-all so that a std:: name from ANOTHER header -- std::swap
# (<utility>), std::accumulate (<numeric>) -- cannot be miscredited to this one.
ALGORITHM_SYMBOLS = (
    "all_of", "any_of", "clamp", "copy", "copy_n", "count", "count_if",
    "fill", "fill_n", "find", "find_if", "for_each", "lower_bound", "max",
    "max_element", "min", "min_element", "minmax", "none_of", "nth_element",
    "partition", "remove_if", "reverse", "rotate", "sort", "stable_sort",
    "transform", "unique", "upper_bound",
)

USES = re.compile(r"\bstd::(?:%s)\b" % "|".join(ALGORITHM_SYMBOLS))
INCLUDES = re.compile(r"^\s*#\s*include\s*<algorithm>")

# Strings are stripped before matching: a log message naming std::max is prose,
# not a call. Escaped quotes are rare enough here that the simple form is right.
STRING = re.compile(r'"(?:[^"\\]|\\.)*"')

# An inline escape hatch for the rare deliberate case.
ALLOW_COMMENT = "check_includes: allow"


def scan_file(path):
    """Return (first line number using an <algorithm> symbol, that symbol),
    or None when the file either does not use one or includes the header."""
    hit = None
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for n, line in enumerate(f, 1):
            if INCLUDES.match(line):
                return None
            if hit is not None:
                continue
            if ALLOW_COMMENT in line:
                continue
            s = line.lstrip()
            if s.startswith("//") or s.startswith("*"):
                continue
            m = USES.search(STRING.sub('""', line))
            if m:
                hit = (n, m.group(0))
    return hit


def main():
    failures = []
    for base in SCAN:
        for dirpath, _dirs, names in os.walk(os.path.join(ROOT, base)):
            for name in sorted(names):
                if not name.endswith(EXTS):
                    continue
                path = os.path.join(dirpath, name)
                hit = scan_file(path)
                if hit:
                    rel = os.path.relpath(path, ROOT).replace(os.sep, "/")
                    failures.append((rel, hit[0], hit[1]))

    if not failures:
        print("check_includes: OK -- every <algorithm> user includes it")
        return 0

    failures.sort()
    print("check_includes: %d file(s) use an <algorithm> symbol without "
          "including <algorithm>.\nThey compile on libstdc++ and break on "
          "Apple clang's libc++:\n" % len(failures))
    for rel, n, what in failures:
        print("  %s:%d: %s" % (rel, n, what))
    print("\nAdd  #include <algorithm>  to each file's own include block.")
    print("If a hit is a false positive, append a '// %s' comment."
          % ALLOW_COMMENT)
    return 1


if __name__ == "__main__":
    sys.exit(main())
