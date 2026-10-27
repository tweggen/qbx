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

CORRECTION, 2026-10-27 (QBX-246), measured rather than reasoned: THE PARAGRAPH
ABOVE IS NO LONGER TRUE, and it is kept because the reason it stopped being
true is the useful part. Proposal 45 D9 hardened stringToPath so that an
unparsable component becomes SPATH_INVALID rather than 0, and resolveByPath
returns nullptr on SPATH_INVALID (sobjectpath.h, "an unparsable one
(SPATH_INVALID) resolves to nothing"). So:

    stringToPath( "Drums:0,0" )  ->  ( SPATH_INVALID, 0 )  ->  resolves to NOTHING

A bare parse of a qualified path therefore FAILS CLOSED today. Watched: with
the shell's seams reverted to rootContainer() plus a bare stringToPath(),
`assert-track-detail-sections trackPath="Drums:1"` did not read the master's
lane -- it answered nothing and the verb refused.

THE RULE IS UNCHANGED AND THIS CHECKER STILL EARNS ITS PLACE; only the severity
moved. A bare parse of a qualifiable path is a DEAD gesture, not a gesture that
edits the wrong tree. The wrong-tree half of the class is real but belongs to
its sibling, tools/check_root_resolution.py: there a BARE path such as "0" is
resolved against the MASTER while the object lives in an arrangement, and index
{0} exists in every root, so that one does succeed on the wrong object. Do not
repeat the pre-D9 story for this checker, as QBX-242 did.

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
    # TWO gaps, nine sites, and the count is stated so a tenth cannot arrive
    # unnoticed behind a reason that only covers the first nine (QBX-246 found
    # this entry describing 8 of what were already 9). The describe/grab/click
    # seams that used to be in here are gone: they resolve laneBySpec() now.
    "shell/src/smainwindow.cpp":
        "GAP x8 (QBX-244): eventEditor_->bindClip takes a bare path, so an "
        "event clip in an arrangement is not reachable from the event editor "
        "yet. GAP x1 (QBX-243): isolateTakeGesture parses clipRef bare",

    # ---- ADDED BY QBX-242, when this checker was finally wired into
    # ---- ci/gates.sh and ran for the first time since 2026-08-20.
    #
    # The four below PEEL THE QUALIFIER THEMSELVES and then split what is left,
    # so every token they hand to stringToPath() is bare by construction. That
    # is the same reason sobjectpath.h is exempt, one layer up.
    "model/src/splacements.cpp":
        "laneBySpec() peels the root qualifier into rootInOut and parses only "
        "the already-stripped remainder -- the same split sobjectpath.h does",
    "objects/mixer/src/spackclipsaction.cpp":
        "readXml peels '<root>:' into pathRoot_ before splitting on ';', so the "
        "tokens are bare by construction",
    "objects/mixer/src/spackselectionaction.cpp":
        "readXml peels '<root>:' into pathRoot_ before splitting on ';', so the "
        "tokens are bare by construction",
    "objects/cut/src/scleanuptakesaction.cpp":
        "the clips= list is relative to the root already resolved from "
        "pathRoot via splacements::rootNamed(), so the entries are bare",

    # The three below are REAL DEFECTS with a filed owner, carried as gaps in
    # the idiom the smainwindow entry above established. They are not safe and
    # they are not approved: each parses a path master-rooted AND resolves it
    # against the master, so the two halves agree and the defect is invisible.
    # Delete the entry when the ticket lands; do not widen it.
    "objects/track/src/slearnfeelflowaction.cpp":
        "GAP (QBX-245): learn-feel-flow has no pathRoot at all -- it parses "
        "trackPath bare and resolves it against the master",
    "objects/track/src/ssetfeelflowmodeaction.cpp":
        "GAP (QBX-245): set-feel-flow-mode has no pathRoot at all -- it parses "
        "trackPath bare and resolves it against the master",

    # ssidecartestactions.cpp's entry was DELETED by QBX-246, which is what an
    # exemption carrying a ticket is for: feelFlowTrackAt() and
    # feel-flow-analyze now parse with parseQualified()/parseInto() and resolve
    # with rootNamed(), so the file has no bare parse left to exempt. Deleting
    # the entry is the last step of the fix, not an afterthought -- an
    # exemption left behind re-opens the hole silently for the next change.
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
