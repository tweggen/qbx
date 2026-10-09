#!/usr/bin/env python3
"""check_product_name.py -- the product name is a variable; its identity keys are not.

Plan 51 (QBX-137). Two jobs, and the second is the one that matters.

JOB 1 -- no new display literals. A capital-`Smaragd` inside a C/C++ string
literal in application or engine code is a user-visible name hard-coded past
the indirection. Exempt a line with a trailing

    // check_product_name: allow -- <why>

JOB 2 -- the frozen identity keys are pinned. These are not names, they are
where the user's settings, saved passwords, MIDI routing and installed bundle
live (plan 51 §3). Renaming one is a silent data migration: the app starts
cleanly against an empty location and merely looks forgetful. So each is
pinned by file, pattern AND EXPECTED COUNT -- without the count, a refactor
that drops an occurrence passes quietly.

Job 2 is what replaces the runtime gate this check was first designed around.
With the name a compile-time constant, no test can configure a DIFFERENT name,
so "change it and assert the paths did not move" had nothing to turn. A static
pin needs no running app and catches the regression a diff reviewer cannot see:
someone sweeping "Smaragd" and catching an identity key in the net.

    python3 tools/check_product_name.py [--warn-only]

--warn-only downgrades JOB 1 to warnings; job 2 always fails. That is what lets
the identity-key protection land before the display sweep (plan 51 §6 stage 2).
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

ALLOW_COMMENT = "check_product_name: allow"

# Job 1 searches these trees...
SEARCH_DIRS = ["smaragd/main", "smaragd/tw303a"]
SOURCE_EXT = (".cpp", ".cc", ".c", ".h", ".hpp", ".mm")

# ...and skips these. Note testkit/ is NOT skipped wholesale: its verbs ship
# inside smaragd.exe and are application code, so a hard-coded caption in one
# must not slip through. Only the test binaries in it are exempt.
def is_skipped(rel):
    parts = rel.split(os.sep)
    if "tests" in parts:                      # **/tests/**
        return True
    if rel.startswith("smaragd" + os.sep + "tests"):
        return True
    base = os.path.basename(rel)
    if base.endswith("_test.cpp") or base.endswith("_test.cc"):
        return True
    return False


def string_literals(text):
    """Yield (line_no, literal) for every string literal, COMMENTS REMOVED.

    A line filter is not enough and the repo proves it: smainwindow.h carries
    a string literal INSIDE a // comment (`// post-exec() "Smaragd exiting"
    line`). Stripping comments with a regex is equally wrong, because a
    literal may contain // (every URL does). So walk the characters and track
    which of the four states we are in.
    """
    i, n, line = 0, len(text), 1
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
        elif c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                if text[i] == "\n":
                    line += 1
                i += 1
            i += 2
        elif c == "'":                         # char literal; may contain '\''
            i += 1
            while i < n and text[i] != "'":
                i += 2 if text[i] == "\\" else 1
            i += 1
        elif c == '"':
            start_line = i + 1
            begin = line
            i += 1
            buf = []
            while i < n and text[i] != '"':
                if text[i] == "\\":
                    buf.append(text[i:i + 2])
                    i += 2
                    continue
                if text[i] == "\n":
                    line += 1
                buf.append(text[i])
                i += 1
            i += 1
            del start_line
            yield begin, "".join(buf)
        else:
            i += 1


def job1(warn_only):
    """Capital-Smaragd in a string literal, outside the allowed sites."""
    hits = []
    for d in SEARCH_DIRS:
        for dirpath, _dirnames, filenames in os.walk(os.path.join(ROOT, d)):
            for fn in sorted(filenames):
                if not fn.endswith(SOURCE_EXT):
                    continue
                path = os.path.join(dirpath, fn)
                rel = os.path.relpath(path, ROOT)
                if is_skipped(rel):
                    continue
                with open(path, encoding="utf-8", errors="replace") as fh:
                    text = fh.read()
                lines = text.splitlines()
                for lineno, lit in string_literals(text):
                    # Case-sensitive on the CAPITAL form only. The lowercase
                    # name is 35 more literals and they are filenames, log
                    # names and paths -- Class C, which QBX-137 excludes in as
                    # many words. Flagging them would invite someone to rename
                    # files, which is the one thing the ticket rules out.
                    if "Smaragd" not in lit:
                        continue
                    src = lines[lineno - 1] if lineno - 1 < len(lines) else ""
                    if ALLOW_COMMENT in src:
                        continue
                    hits.append((rel, lineno, lit))

    if not hits:
        print("  job 1: no un-marked \"Smaragd\" display literals")
        return 0

    label = "WARNING" if warn_only else "ERROR"
    print("  job 1: %d un-marked \"Smaragd\" literal(s)" % len(hits))
    for rel, lineno, lit in hits:
        shown = lit if len(lit) <= 60 else lit[:57] + "..."
        print("    %s  %s:%d  \"%s\"" % (label, rel, lineno, shown))
    if warn_only:
        print("    (warn-only: plan 51 stage 2 lands this check before the")
        print("     stage 3 sweep, so these are not failures yet)")
        return 0
    print("    Read the product name instead of spelling it:")
    print("      app    -- QGuiApplication::applicationDisplayName()")
    print("      engine -- SMARAGD_PRODUCT_NAME from smaragd_version.h")
    print("    If the literal is deliberate, mark the line:")
    print("      // %s -- <why>" % ALLOW_COMMENT)
    return 1


# (path, human name, regex, expected count)
#
# Every one of these exists in the tree today, which is what makes job 2
# independent of the display sweep. The reasons are in plan 51 §2.
PINS = [
    ("smaragd/main/shell/src/ssettings.cpp", "the settings INI location",
     r'QSettings::IniFormat,\s*QSettings::UserScope,\s*\n?\s*"Smaragd",\s*"smaragd"', 1),
    ("smaragd/main/shell/src/smediaaccountmanager.cpp", "the same INI, second instance",
     r'QSettings::IniFormat,\s*QSettings::UserScope,\s*"Smaragd",\s*"smaragd"', 1),
    ("smaragd/main/shell/src/smediaaccountmanager.cpp", "the live keychain service name",
     r'QStringLiteral\(\s*"com\.smaragd\.media"\s*\)', 1),
    ("smaragd/main/shell/include/app/shell/ssecretstore.h", "the service name's default",
     r'QStringLiteral\(\s*"com\.smaragd\.media"\s*\)', 1),
    ("smaragd/main/shell/src/sapplication.cpp", "QStandardPaths' organisation",
     r'setOrganizationName\(\s*"Smaragd"\s*\)', 1),
    ("smaragd/main/shell/src/sapplication.cpp", "QStandardPaths' application name",
     r'setApplicationName\(\s*"smaragd"\s*\)', 1),
    ("smaragd/main/shell/src/ssecretstore_linux.cpp", "the libsecret schema name",
     r'"com\.smaragd\.SecretStore"', 1),
    ("smaragd/main/CMakeLists.txt", "the macOS bundle identifier",
     r'MACOSX_BUNDLE_GUI_IDENTIFIER\s+"dev\.tweggen\.smaragd"', 1),
    # Each MIDI backend holds exactly THREE capital literals after the stage-2
    # refactor -- the client name and its two port names, as named constants.
    # The count is over the WHOLE FILE on purpose: a re-introduced bare literal
    # anywhere in one of these files is then announced by job 2 as well as
    # job 1, which matters because these two files held twelve copies of three
    # strings before the refactor, spelled three different ways.
    ("smaragd/tw303a/devices/src/alsa_seq_midi.cc", "the ALSA MIDI client and port names",
     r'"Smaragd[^"]*"', 3),
    ("smaragd/tw303a/devices/src/coremidi_midi.cc", "the CoreMIDI client and port names",
     r'"Smaragd[^"]*"', 3),
]


def job2():
    problems = 0
    for rel, what, pattern, want in PINS:
        path = os.path.join(ROOT, rel)
        if not os.path.exists(path):
            print("    ERROR  %s is gone; %s was pinned in it" % (rel, what))
            problems += 1
            continue
        with open(path, encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        got = len(re.findall(pattern, text))
        if got != want:
            print("    ERROR  %s: expected %d match(es) for %s, found %d"
                  % (rel, want, what, got))
            print("           pattern: %s" % pattern)
            problems += 1

    # The product passes the service name explicitly, so the header default is
    # dead code -- until someone drops the explicit argument, at which point
    # the default silently takes over. Pinning only the live site would miss
    # that, so pin both AND assert they agree.
    live = _one(r'QStringLiteral\(\s*"(com\.smaragd\.[a-z]+)"\s*\)',
                "smaragd/main/shell/src/smediaaccountmanager.cpp")
    dflt = _one(r'serviceName\s*=\s*QStringLiteral\(\s*"(com\.smaragd\.[a-z]+)"\s*\)',
                "smaragd/main/shell/include/app/shell/ssecretstore.h")
    if live and dflt and live != dflt:
        print("    ERROR  the keychain service name disagrees with its default:")
        print("           live    %s  (smediaaccountmanager.cpp)" % live)
        print("           default %s  (ssecretstore.h)" % dflt)
        print("           Drop the explicit argument and the default takes over.")
        problems += 1

    if problems:
        print("  job 2: %d frozen identity key(s) MOVED" % problems)
        print("    These are not names. They are where the user's settings,")
        print("    saved passwords, MIDI routing and installed bundle live.")
        print("    Changing one is a data migration, not a rename -- and a")
        print("    SILENT one. See plan/proposed/51_PRODUCT_NAME.md §3.")
        return 1
    print("  job 2: all %d frozen identity keys intact" % len(PINS))
    return 0


def _one(pattern, rel):
    path = os.path.join(ROOT, rel)
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8", errors="replace") as fh:
        m = re.search(pattern, fh.read())
    return m.group(1) if m else None


def main():
    warn_only = "--warn-only" in sys.argv[1:]
    rc = job2()                       # always fatal
    rc |= job1(warn_only)
    return rc


if __name__ == "__main__":
    sys.exit(main())
