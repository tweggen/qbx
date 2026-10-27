#!/usr/bin/env python3
"""check_root_resolution.py -- a widget addresses the root that HOLDS its object.

QBX-242. WHY THIS EXISTS, because the failure mode is silent and has already
cost five tickets:

A project has several ROOTS -- the master (`SProject::masterRoot()`), one per
named arrangement, one per asset fragment. Any code holding a track POINTER
that needs a path, or a path that needs a track, must first answer "which
root?". There are two spellings:

    splacements::rootNamed( project, pathRoot_ )   # ASK: the root is a parameter
    splacements::rootContainer( project )          # ASSUME: the master, always

The action layer is correct by construction: ~60 verbs take the root as a
parameter. The UI, shell and widget layer is where the master gets assumed, and
assuming it is RIGHT only when the question is about the AUDIBLE GRAPH -- the
device, the transport, the recorder, the MIDI out pump, the live monitor --
because only the master is wired to the device.

Everywhere else it is a defect, and it never announces itself: in an
arrangement or asset tab the path resolves to "", every caller returns early
on "", and the gesture does nothing. Not a crash, not a log line -- a dead
button. That is exactly how QBX-230, QBX-231, QBX-232, QBX-233 and QBX-234
were each found: by a human clicking something that did not respond.

WHAT TO DO INSTEAD, when a site is flagged:

  * the root that HOLDS the object  ->  splacements::specOf( project, obj )
                                       (+ rootNamed, or parseQualified for the
                                        name), the inverse of laneBySpec()
  * the root the ACTIVE VIEW edits  ->  stimeline::submitActive() /
                                       submitFor( view, a ) / submitRooted()
  * genuinely the master            ->  keep it, and say so with a marker:

        // check_root_resolution: master -- only the master reaches the device

The marker must carry a REASON. "master -- deliberate" is not one; name what
makes the master the right answer at that site. A site with no marker and no
reason is the state this checker exists to make impossible.

THE SECOND MARKER, for a site that is WRONG and not being fixed in this change:

        // check_root_resolution: suspect QBX-243 -- <what breaks, and where>

It needs a ticket key, and the checker counts these and prints the total on
every run, so the backlog is a number somebody sees rather than a silence. This
is deliberately NOT spelled "allow": the site is a known defect with a filed
owner, not an approved exception. QBX-242's own sweep classified 93 sites and
found ~40 of them suspect -- too many to convert in one change without a gate
per behaviour, so they are enumerated here instead of quietly passing.

A suspect marker is a debt, so the honest end state for this checker is zero of
them. If you are reading this and the count is still large, the follow-up
tickets are the work, not the marker.

ON THE NAME COLLISION, which is why this check could not be written before.
`SObject::getRootComponent()` is a WHOLLY DIFFERENT method -- the DSP component
a lane renders through, not the project's root container -- and it accounts for
most of the ~140 `getRootComponent` mentions in the tree. While SProject spelled
its accessor the same way, no grep and no checker could separate "give me the
master root" from "give me this track's audio component". QBX-242 renamed the
project one to `SProject::masterRoot()` for that reason, and this checker is
what the rename bought.

    python3 tools/check_root_resolution.py

Companion check: tools/check_pathroot.py polices the same class from the
path-PARSING side, and THE TWO HALVES FAIL DIFFERENTLY -- QBX-242 wrote that
they failed the same way, and QBX-246 measured otherwise:

  * a QUALIFIED path parsed bare ("Drums:0" through stringToPath) FAILS CLOSED
    since proposal 45 D9, which gives SPATH_INVALID rather than 0 and makes
    resolveByPath return null. A dead gesture.
  * a BARE path resolved against the MASTER -- what THIS checker polices -- does
    not fail. Index {0} exists in every root, so it resolves a real object in
    the wrong tree and SUCCEEDS. That is the silent half, and it is this one.

The practical consequence when writing a gate: an unknown-root arm such as
trackPath="Nope:0" with expectReject="true" passes on the broken build too,
because the D9 parse refuses it as well. Gate the half you mean.

Both are in ci/gates.sh; a checker that gates.sh does not run is not a gate,
which is how check_pathroot sat unenforced from 2026-08-20 while the class it
was written to stop returned seven times.
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent / "smaragd" / "main"

MARKER = "check_root_resolution: master"

# The suspect marker needs a ticket key, so a known defect has an owner. Without
# one it is just a silencer, which is what this checker exists to prevent.
SUSPECT = re.compile(r"check_root_resolution:\s*suspect\s+([A-Z]+-\d+)\b")

# The two spellings that resolve the master unconditionally.
PATTERNS = [
    (re.compile(r"\bsplacements::rootContainer\s*\("), "splacements::rootContainer()"),
    (re.compile(r"(->|\.)\s*masterRoot\s*\(\s*\)"), "SProject::masterRoot()"),
]

# file -> reason. These DEFINE the master-resolution service or are the one
# place that legitimately enumerates every root starting with the master.
EXEMPT = {
    "model/include/app/model/splacements.h":
        "defines rootContainer() and rootNamed(); the master is what they "
        "resolve, so naming it here is the point",
    "model/src/splacements.cpp":
        "specOf() and laneBySpec() try the master FIRST and then every named "
        "root -- enumerating the roots is their job",
}

SKIP_SUFFIX = ("_test.cpp",)   # unit-test binaries, not shipped UI code


def reason_after_marker(text: str) -> str:
    """The prose a marker carries, or '' when it carries none."""
    i = text.find(MARKER)
    if i < 0:
        return ""
    rest = text[i + len(MARKER):].lstrip()
    rest = rest.lstrip("-–—:").strip()
    return rest


def main() -> int:
    unmarked: list[str] = []
    reasonless: list[str] = []
    suspects: dict[str, list[str]] = {}
    marked = 0

    files = sorted(ROOT.rglob("*.cpp")) + sorted(ROOT.rglob("*.h")) \
        + sorted(ROOT.rglob("*.cc"))
    for p in files:
        rel = p.relative_to(ROOT).as_posix()
        if rel in EXEMPT or rel.endswith(SKIP_SUFFIX):
            continue
        lines = p.read_text(encoding="utf-8", errors="replace").splitlines()
        for n, line in enumerate(lines, 1):
            stripped = line.lstrip()
            if stripped.startswith(("//", "*", "/*")):
                continue
            hit = next((name for pat, name in PATTERNS if pat.search(line)), None)
            if not hit:
                continue
            # The marker may sit on the line itself or on the line above, so a
            # long call can carry it without going past the column limit.
            above = lines[n - 2] if n >= 2 else ""
            sus = SUSPECT.search(line) or SUSPECT.search(above)
            if sus:
                suspects.setdefault(sus.group(1), []).append(f"{rel}:{n}")
            elif MARKER in line or MARKER in above:
                carrier = line if MARKER in line else above
                if reason_after_marker(carrier):
                    marked += 1
                else:
                    reasonless.append(f"{rel}:{n}: {stripped}")
            else:
                unmarked.append(f"{rel}:{n}: [{hit}] {stripped}")

    if unmarked or reasonless:
        print("check_root_resolution: a site resolves the MASTER root without "
              "saying why.")
        print()
        print("  Only the master is wired to the device, so assuming it is "
              "correct ONLY for")
        print("  the audible graph. Anywhere else, an object in an arrangement "
              "or asset tab")
        print("  resolves to \"\", the caller returns early, and the gesture "
              "dies silently.")
        print()
        print("  Address the root that HOLDS the object with "
              "splacements::specOf(), or the")
        print("  root the active view edits with stimeline::submitActive() / "
              "submitRooted().")
        print("  If the master really is the answer, mark the line:")
        print()
        print(f"      // {MARKER} -- <why the master is right here>")
        print()
        if unmarked:
            print(f"  UNMARKED ({len(unmarked)}):")
            for b in unmarked:
                print("    " + b)
            print()
        if reasonless:
            print(f"  MARKED BUT WITH NO REASON ({len(reasonless)}):")
            for b in reasonless:
                print("    " + b)
            print()
        return 1

    total_sus = sum(len(v) for v in suspects.values())
    print(f"check_root_resolution: OK -- {marked} master-root site(s), each "
          f"with a stated reason")
    if suspects:
        # Printed on EVERY run, not tucked behind a flag. A debt nobody sees is
        # how check_pathroot sat unwired for two months (QBX-242).
        print(f"  {total_sus} site(s) still carry a SUSPECT marker -- known "
              f"defects awaiting their ticket:")
        for t in sorted(suspects):
            print(f"    {t}: {len(suspects[t])} site(s)")
        print("  These are debts, not exceptions. The honest end state is zero.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
