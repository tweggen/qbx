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

================================================================================
JOB 2 (QBX-249): AND THE ROOT LIVES IN SAction::pathRoot_, NOWHERE ELSE.
================================================================================

Parsing the qualifier correctly is half of it. The other half is WHERE the
answer is kept, because two funnels read that one member and nothing else:

    applyPropagatingRoot()  copies a->pathRoot() onto every inverse, which is
                            how 83 inverse-construction sites across 36 files
                            get their root without one of them saying so.
    setPathRoot()           is what stimeline::submitFor(), SMixerStrip and
                            SEventPassVerbs::emitVerb stamp an action with.

An action that keeps its root in a member of its own is invisible to both. It
is not a style question; it is how QBX-249 happened. `add-track` held an
`arrangement_`, so:

  * the SRemoveTrackAction it handed back as its inverse was MASTER-rooted,
    and one undo of an arrangement-side add-track DELETED A MASTER LANE while
    leaving the arrangement's standing -- measured, and silent, because
    remove-arrangement then correctly refused the still-populated root;
  * setPathRoot() was ignored, so "New track" in an arrangement tab added the
    lane to the master while the view looked for it in its own mixer.

`add-sample` (undo destroyed the master's clip at the same address, and
writeXml dropped the root entirely) and `place-asset` had the same defect.
`merge-columns` and `set-lane-hidden` had the sibling form: a pathRoot_ of
their own SHADOWING the base's, so readXml wrote one member and setPathRoot()
the other.

So: a class deriving from SAction must not declare a root of its own, and must
never redeclare pathRoot_ or pathRoot(). This runs over the headers that
declare an SAction subclass, which is what makes it cheap and exact.

NOT THE TESTKIT, and the reason is measured rather than assumed: no testkit
verb carrying an `arrangement_`/`root_` returns an inverse (checked -- zero
`return {true, new S...Action}` across all six), and nothing stamps one with
setPathRoot(), so neither funnel reaches them. A SHADOW is still refused
there, because a shadow is never a design choice.

Job 2 has NO exemptions, and that is the point of having migrated the six
send verbs that were already correct: a rule with six exceptions is the shape
that let this class come back.
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

# Job 2. A class deriving from SAction keeps its root in the BASE's pathRoot_.
IS_ACTION = re.compile(r":\s*public\s+SAction\b")
# A root of the class's own, under any of the names this tree has used for one:
# arrangement_, destRoot_, containerRoot_, rootName_.
OWN_ROOT = re.compile(
    r"\bQString\s+[A-Za-z_]*(?:[Aa]rrangement|[Rr]oot)[A-Za-z_]*_\s*;")
# And the sibling form: the base's own member or accessor, redeclared.
SHADOW = re.compile(r"\bpathRoot_\s*;|\bpathRoot\s*\(\s*\)\s*const")


def job_bare_parse() -> int:
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
        print("check_pathroot job 1: stringToPath() on a path that may be")
        print("  root-qualified. Use strackpath::parseQualified() / parseInto()")
        print("  instead, or add an explicit exemption with a reason in")
        print("  tools/check_pathroot.py.")
        print()
        for b in bad:
            print("  " + b)
        return 1

    print("check_pathroot job 1: OK -- no unqualified path parses")
    return 0


def job_root_member() -> int:
    bad = []
    n_headers = 0
    for p in sorted(ROOT.rglob("*.h")):
        rel = p.relative_to(ROOT).as_posix()
        text = p.read_text(encoding="utf-8", errors="replace")
        if not IS_ACTION.search(text):
            continue            # not an action header; its members are its own
        n_headers += 1
        testkit = rel.startswith("testkit/")
        for n, line in enumerate(text.splitlines(), 1):
            if "check_pathroot" in line or line.lstrip().startswith(("//", "*")):
                continue
            if SHADOW.search(line):
                bad.append(f"{rel}:{n}: shadows SAction::pathRoot -- {line.strip()}")
            elif OWN_ROOT.search(line) and not testkit:
                bad.append(f"{rel}:{n}: a root of its own -- {line.strip()}")

    if bad:
        print("check_pathroot job 2: an SAction subclass keeping its root")
        print("  somewhere applyPropagatingRoot() and setPathRoot() cannot see.")
        print("  Use the base's pathRoot_ (set it with setPathRoot() from a")
        print("  constructor; a base member cannot go in a member-init list).")
        print("  QBX-249 and the docstring say what a root kept elsewhere costs.")
        print()
        for b in bad:
            print("  " + b)
        return 1

    print(f"check_pathroot job 2: OK -- {n_headers} action header(s), "
          "every root in SAction::pathRoot_")
    return 0


def main() -> int:
    return job_bare_parse() | job_root_member()

if __name__ == "__main__":
    sys.exit(main())
