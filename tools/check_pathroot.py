#!/usr/bin/env python3
"""Root-qualified paths must not be parsed with stringToPath() (proposal 09 D21).

WHY THIS EXISTS, in one paragraph, because the failure mode is invisible:

    stringToPath( "Drums:0,0" )  ->  ( "Drums:0".toInt(), 0 )  ->  ( 0, 0 )

stringToPath splits on COMMAS. Handed a root-qualified path it does not fail --
it silently yields a MASTER-rooted one, and index {0} exists in every root, so
the caller then resolves a real object in the wrong tree and succeeds. That cost
three sessions once already: assert-clip-window read the master's clip while the
edit under test correctly changed the arrangement's, and the conclusion drawn
was "the gesture does not land".

The rule: any path that can carry a qualifier is parsed with
strackpath::parseQualified() or strackpath::parseInto(). stringToPath() is for
a path that is known-bare by construction.

Exemptions are listed explicitly below with a reason each. Add one only when the
path genuinely cannot be qualified -- not to silence the check.
"""
import re, sys, pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent / "smaragd" / "main"

# path -> reason. These pass a path they BUILT themselves, or one already split.
EXEMPT = {
    # THE DEFINITION ITSELF, and parseQualified() calling it on the already-
    # stripped remainder. This is where a bare path is legitimately parsed.
    "model/include/app/model/sobjectpath.h":
        "defines stringToPath and is what parseQualified calls once the "
        "qualifier is stripped",

    # extract-arrangement's trackPaths are MASTER-rooted by definition: you
    # extract tracks OUT of the master, and the arrangement does not exist yet
    # when the paths are named. dissolve's restorePlan is the mirror -- the
    # destinations it names are where the tracks GO, which is the master tree.
    "objects/mixer/src/sextractarrangementaction.cpp":
        "trackPaths name tracks in the MASTER; the arrangement does not exist "
        "yet when they are parsed",
    "objects/mixer/src/sdissolvearrangementaction.cpp":
        "restorePlan names MASTER destinations (where the tracks go back to)",

    # KNOWN GAP, not a safe path: the event editor dock binds a clip by bare
    # index path and has no root of its own, so an event clip inside an
    # ARRANGEMENT cannot be opened in it. Listed here rather than silently
    # passing, because an exemption with a reason is the honest way to carry a
    # gap that is not being fixed in this change. See the PR body.
    "shell/src/smainwindow.cpp":
        "GAP: eventEditor_->bindClip takes a bare path; an event clip in an "
        "arrangement is not reachable from the event editor yet",
}

PAT = re.compile(r"\bstringToPath\s*\(")

def main() -> int:
    bad = []
    for p in sorted(ROOT.rglob("*.cpp")) + sorted(ROOT.rglob("*.h")):
        rel = p.relative_to(ROOT).as_posix()
        if rel in EXEMPT:
            continue
        for n, line in enumerate(p.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            if "check_pathroot" in line or line.lstrip().startswith(("//", "*")):
                continue
            if PAT.search(line):
                bad.append(f"{rel}:{n}: {line.strip()}")

    if bad:
        print("check_pathroot: stringToPath() on a path that may be root-qualified.")
        print("  Use strackpath::parseQualified() / parseInto() instead, or add an")
        print("  explicit exemption with a reason in tools/check_pathroot.py.")
        print()
        for b in bad:
            print("  " + b)
        return 1

    print("check_pathroot: OK -- no unqualified path parses")
    return 0

if __name__ == "__main__":
    sys.exit(main())
