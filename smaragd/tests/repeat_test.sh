#!/bin/bash
# Run a single .qxa test case N times and report the pass rate — the
# determinism gate for proposal 19 (async freeze model). A flaky freeze race
# shows up here as <N/N; a fixed model must be N/N.
#
# Sample paths in a .qxa resolve relative to the .qxa's own directory (see
# SProject::setSampleBaseDir), so this works from any working directory.
#
# Usage:
#   ./repeat_test.sh <smaragd_binary> <test.qxa> [N] [reval_workers]
# Example (from anywhere):
#   ./repeat_test.sh ../build/bin/smaragd.app/Contents/MacOS/smaragd \
#       ./cases/takes_group_broadcast.qxa 100

set -u

SMARAGD="${1:?Usage: $0 <smaragd_binary> <test.qxa> [N] [reval_workers]}"
TESTCASE="${2:?Usage: $0 <smaragd_binary> <test.qxa> [N] [reval_workers]}"
N="${3:-100}"
WORKERS="${4:-}"

if [ ! -x "$SMARAGD" ]; then
    echo "Error: $SMARAGD is not executable"; exit 2
fi
if [ ! -f "$TESTCASE" ]; then
    echo "Error: $TESTCASE not found"; exit 2
fi

OUTDIR="$(mktemp -d)"
trap 'rm -rf "$OUTDIR"' EXIT

[ -n "$WORKERS" ] && export SMARAGD_REVAL_WORKERS="$WORKERS"

pass=0; fail=0
firstfail=""
for i in $(seq "$N"); do
    if "$SMARAGD" --test-case "$TESTCASE" --test-output-dir "$OUTDIR" 2>/dev/null \
        | grep -q "^PASS - "; then
        pass=$((pass+1))
    else
        fail=$((fail+1))
        [ -z "$firstfail" ] && firstfail="$i"
    fi
done

echo ""
echo "$(basename "$TESTCASE"): $pass/$N passed${WORKERS:+  (SMARAGD_REVAL_WORKERS=$WORKERS)}"
if [ "$fail" -gt 0 ]; then
    echo "  FLAKY/FAILING: $fail failure(s), first at iteration $firstfail"
    exit 1
fi
echo "  deterministic: PASS"
exit 0
