#!/bin/bash
# Proposal 21 L5 flake sweep. Like repeat_test.sh but judged on the EXIT CODE
# rather than on the "PASS - " line: repeat_test.sh greps stdout, so a case that
# passes and then crashes during TEARDOWN counts as a pass there while ctest —
# which judges by exit code — fails it (CLAUDE.md, "two known crash flakes").
#
# Run from smaragd/tests/cases/ — the fixture paths in a .qxa are CWD-relative.
#
#   ../sweep_l5.sh <case.qxa> <N> <env...>
set -u
CASE="${1:?usage: sweep_l5.sh <case.qxa> <N> [KEY=VAL ...]}"
N="${2:-50}"
shift 2

BIN=../../build/bin/smaragd.exe
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

for W in 1 4 8 16; do
    pass=0; fail=0; first=""
    for i in $(seq "$N"); do
        if env "$@" SMARAGD_REVAL_WORKERS="$W" QT_QPA_PLATFORM=offscreen \
             "$BIN" --test-case "$CASE" --test-output-dir "$OUT" >/dev/null 2>&1
        then pass=$((pass+1))
        else fail=$((fail+1)); [ -z "$first" ] && first="$i"
        fi
    done
    echo "SWEEP $CASE workers=$W: $pass/$N${first:+  first failure at $first}"
done
