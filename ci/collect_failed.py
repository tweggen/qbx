#!/usr/bin/env python3
"""ci/collect_failed.py -- gather what a FAILED ctest case left behind (QBX-322).

    python3 ci/collect_failed.py <build-dir> <dest-dir>

For every test that did not pass in the last ctest run, writes

    <dest-dir>/<test-name>/run.log     that test's own block of LastTest.log:
                                       its command line, its whole output
                                       (with SMARAGD_LOG_CONSOLE=1, the case's
                                       own warnings) and its exit status
    <dest-dir>/<test-name>/output/...  a copy of its --test-output-dir:
                                       playback-capture dumps, renders,
                                       screenshots, saved projects

and nothing for a test that passed. CI uploads <dest-dir> as a workflow
artifact when the gate fails, so a failing value can be read from the .wav
itself instead of being decoded from six printed digits (QBX-318).

Why LastTest.log and not LastTestsFailed.log: LastTest.log is rewritten by
every run and carries each test's output and status together, so this script
needs no second file whose freshness it would have to trust. Each block in
LastTest.log is written whole when its test finishes, so under -j the blocks
interleave but never mix.

Bounded: a broken build can fail every case, and the suite's outputs total
~630 MB. Output dirs are copied in failure order until MAX_BYTES; past that a
case gets its run.log only, and SKIPPED.txt names what was left out.

Stdlib only, like the static checkers, so it runs wherever ci/gates.sh does.
"""
import os
import re
import shutil
import sys

MAX_BYTES = 200 * 1024 * 1024

# "266/645 Testing: qxa.midi_record_modes" opens a block; the status line
# ("Test Passed.", "Test Failed.", "***Timeout ...", "Exception: ...") follows
# the "<end of output>" marker; '"<name>" time elapsed:' closes it.
OPEN = re.compile(r'^\d+/\d+ Testing: (\S+)\s*$')
OUTDIR_ARG = re.compile(r'"--test-output-dir" "([^"]+)"')
OUTDIR_DEF = re.compile(r'"-DOUTDIR=([^"]+)"')  # the cmake -P xproc case


def blocks(log_path):
    name, lines = None, []
    with open(log_path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = OPEN.match(line)
            if m:
                name, lines = m.group(1), [line]
                continue
            if name is None:
                continue
            lines.append(line)
            if line.startswith('"%s" time elapsed:' % name):
                yield name, lines
                name, lines = None, []
    if name is not None:  # ctest killed mid-test: keep what there is
        yield name, lines


def passed(lines):
    after = False
    for line in lines:
        if line.startswith("<end of output>"):
            after = True
        elif after and line.strip() == "Test Passed.":
            return True
    return False


def tree_bytes(path):
    total = 0
    for root, _, files in os.walk(path):
        for fn in files:
            try:
                total += os.path.getsize(os.path.join(root, fn))
            except OSError:
                pass
    return total


def main():
    if len(sys.argv) != 3:
        print(__doc__.strip().splitlines()[2], file=sys.stderr)
        return 2
    build, dest = sys.argv[1], sys.argv[2]
    log = os.path.join(build, "Testing", "Temporary", "LastTest.log")
    if not os.path.isfile(log):
        print("collect_failed: no %s, nothing to collect" % log)
        return 0

    shutil.rmtree(dest, ignore_errors=True)
    used, skipped, count = 0, [], 0
    for name, lines in blocks(log):
        if passed(lines):
            continue
        count += 1
        case_dir = os.path.join(dest, name)
        os.makedirs(case_dir, exist_ok=True)
        with open(os.path.join(case_dir, "run.log"), "w", encoding="utf-8") as f:
            f.writelines(lines)

        text = "".join(lines)
        m = OUTDIR_ARG.search(text) or OUTDIR_DEF.search(text)
        out = m.group(1) if m else None
        if not out or not os.path.isdir(out):
            print("  %-48s run.log (no output dir)" % name)
            continue
        size = tree_bytes(out)
        if used + size > MAX_BYTES:
            skipped.append("%s  %d bytes  %s" % (name, size, out))
            print("  %-48s run.log only (output %.1f MB over the cap)"
                  % (name, size / 1e6))
            continue
        shutil.copytree(out, os.path.join(case_dir, "output"))
        used += size
        print("  %-48s run.log + output (%.1f MB)" % (name, size / 1e6))

    if skipped:
        with open(os.path.join(dest, "SKIPPED.txt"), "w", encoding="utf-8") as f:
            f.write("Output dirs left out past the %d-byte cap:\n" % MAX_BYTES)
            f.write("\n".join(skipped) + "\n")
    print("collect_failed: %d failed test(s), %.1f MB of output in %s"
          % (count, used / 1e6, dest))
    return 0


if __name__ == "__main__":
    sys.exit(main())
