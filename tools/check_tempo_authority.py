#!/usr/bin/env python3
"""twTempoMap is THE tempo authority; nothing else may store tempo.

Proposal 37 P1 established the rule and left it as a documented grep in
main/objects/midi/CONTRACT.md: `bpmTempo_` is gone, `set-tempo` is the ONE
write, and `SProject::getBPMTempo()` is a derived view. Tempo is stored as
SMF's own unit -- microseconds per quarter, an integer -- so BPM and the map
cannot disagree; a stored `60/bpm` seconds-per-beat and a stored us/quarter
differ in the tenth microsecond, which lands on a frame boundary in a long
project.

Proposal 45 M6 / D7 gives that rule a second thing to guard. A CONDUCTOR LANE
is a system lane for tempo, time signature and markers -- and M6 deliberately
builds only the CONTAINER. Its content must be a VIEW of twTempoMap and never
a second store; doing that properly needs a curve model for ramps and is a
proposal of its own. The failure this checker exists to prevent is a later
milestone quietly giving STrack a tempo field because the conductor lane
"obviously needs one", which is how one authority becomes two.

A DOCUMENTED GREP NOBODY RUNS ROTS, which is why this is a script rather than
another paragraph in a CONTRACT. It enforces two things:

  1. NO TEMPO MEMBER on the app-side lane classes. A member declaration whose
     name is tempo-, bpm- or time-signature-shaped, in the track or mixer
     slices, is a second store by definition.
  2. THE WRITERS OF THE PROJECT'S TEMPO STAY CONFINED. `setBPMTempo(` and
     `bpmTempo_ =` may appear only in the files listed below -- the SProject
     accessor itself, its loader, and the one action verb.

Run from the repo root:  python tools/check_tempo_authority.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Rule 1: where a tempo MEMBER would land if somebody added one.
MEMBER_SCAN = [
    os.path.join("smaragd", "main", "objects", "track"),
    os.path.join("smaragd", "main", "objects", "mixer"),
]

# A member declaration: a trailing `<name>_;` or `<name>_ =` whose identifier
# names tempo ANYWHERE in it, not only at the start. That distinction is the
# whole value of the rule and the first version got it wrong: `conductorTempo_`
# -- the single most likely name for the field this checker exists to forbid --
# sailed straight past a pattern anchored at a word boundary. Watched failing
# on exactly that member.
#
# The trailing underscore is what keeps it narrow: it must not fire on a LOCAL
# named `tempoMap`, on a parameter, or on the many legitimate READS
# (`p->tempoMap()`, `slot->setTempoMap(...)`).
MEMBER = re.compile(
    r"\b\w*(?:tempo|bpm|beatsPerMinute|usPerQuarter|microsPerQuarter"
    r"|timeSig|timeSignature)\w*_\s*(?:;|=[^=])",
    re.IGNORECASE,
)

# A HANDLE TO THE AUTHORITY IS NOT A SECOND STORE. A member that caches the
# project's own twTempoMap is a reference to the one authority and is exactly
# what D7 asks for ("a VIEW of twTempoMap"); it is the VALUE-shaped names --
# a double, a us-per-quarter integer, a numerator -- that make two authorities.
MEMBER_EXEMPT = re.compile(r"\w*(?:tempoMap|TempoMap)\w*_", re.IGNORECASE)

# Rule 2: the project's tempo writers, and the only files allowed to hold one.
WRITER = re.compile(r"\bsetBPMTempo\s*\(|\bbpmTempo_\s*=[^=]")
WRITER_ALLOWED = {
    # The accessor itself and the loader that feeds it.
    "smaragd/main/model/src/sproject.cpp",
    "smaragd/main/model/include/app/model/sproject.h",
    # `set-tempo`, the ONE write, and it is an action.
    "smaragd/main/objects/midi/src/smidiclipactions.cpp",
}

EXTS = (".c", ".cc", ".cpp", ".h", ".hh")
ALLOW_COMMENT = "check_tempo_authority: allow"


def code_lines(path):
    """Yield (lineno, text) for lines that are not comments."""
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for n, line in enumerate(f, 1):
            s = line.lstrip()
            if s.startswith("//") or s.startswith("*") or s.startswith("/*"):
                continue
            if ALLOW_COMMENT in line:
                continue
            yield n, line


def walk(base):
    for dirpath, _dirs, names in os.walk(os.path.join(ROOT, base)):
        for name in sorted(names):
            if name.endswith(EXTS):
                yield os.path.join(dirpath, name)


def rel(path):
    return os.path.relpath(path, ROOT).replace(os.sep, "/")


def main():
    failures = []

    for base in MEMBER_SCAN:
        for path in walk(base):
            for n, line in code_lines(path):
                m = MEMBER.search(line)
                if m and not MEMBER_EXEMPT.fullmatch(
                        m.group(0).strip().rstrip(";=").strip()):
                    failures.append(
                        "%s:%d: a TEMPO MEMBER (%s). twTempoMap is the one "
                        "authority (proposal 37 P1 / 45 D7); a conductor lane's "
                        "content must be a VIEW of it, never a second store."
                        % (rel(path), n, m.group(0).strip().rstrip(";=").strip()))

    for base in [os.path.join("smaragd", "main")]:
        for path in walk(base):
            r = rel(path)
            if r in WRITER_ALLOWED:
                continue
            for n, line in code_lines(path):
                if WRITER.search(line):
                    failures.append(
                        "%s:%d: writes the project tempo. `set-tempo` is the "
                        "ONE write and it is an action -- being an action is "
                        "what keeps undo exact by LIFO, and it is what "
                        "re-derives startTime for every timebase='beats' link."
                        % (r, n))

    if not failures:
        print("check_tempo_authority: OK -- twTempoMap is the one authority")
        return 0

    print("check_tempo_authority: %d violation(s).\n" % len(failures))
    for f in failures:
        print("  " + f)
    print("\nIf a hit is a false positive, append a '// %s' comment."
          % ALLOW_COMMENT)
    return 1


if __name__ == "__main__":
    sys.exit(main())
