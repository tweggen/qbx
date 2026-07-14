#!/bin/bash
# Run all action script tests in a directory.
# Usage: ./run_all_tests.sh <path/to/smaragd> [test_dir] [output_dir]
# Example:
#   ./run_all_tests.sh ../smaragd/build/bin/smaragd.app/Contents/MacOS/smaragd \
#       ../smaragd/tests/cases
#
# Notes:
#  - The binary is run with the test dir as CWD so tests that reference fixtures
#    by relative path (e.g. "../test_sawtooth.wav") resolve correctly.
#  - An output dir is passed via -o so render/screenshot artifacts have a home
#    (and, together with --test-case, keeps the run non-interactive: no modal
#    dialogs block the suite).
#  - Pass/fail is taken from the process exit code (0 = PASS, non-zero = FAIL),
#    which is robust regardless of interleaved debug output.

SMARAGD="${1:?Usage: $0 <smaragd_binary> [test_dir] [output_dir]}"
TEST_DIR="${2:-.}"
OUTPUT_DIR="${3:-$(mktemp -d)}"

# Resolve to absolute paths so cd'ing into the test dir doesn't break them.
SMARAGD="$(cd "$(dirname "$SMARAGD")" && pwd)/$(basename "$SMARAGD")"

if [ ! -x "$SMARAGD" ]; then
    echo "Error: $SMARAGD is not executable"
    exit 1
fi

if [ ! -d "$TEST_DIR" ]; then
    echo "Error: $TEST_DIR is not a directory"
    exit 1
fi

TEST_DIR="$(cd "$TEST_DIR" && pwd)"
mkdir -p "$OUTPUT_DIR"
OUTPUT_DIR="$(cd "$OUTPUT_DIR" && pwd)"

passed=0
failed=0
total=0
fails=""

# Run each test with the test dir as CWD (relative fixture paths resolve there).
for testfile in "$TEST_DIR"/*.qxa; do
    if [ ! -f "$testfile" ]; then
        continue
    fi

    total=$((total + 1))
    testname=$(basename "$testfile")

    if ( cd "$TEST_DIR" && "$SMARAGD" --test-case "$testname" -o "$OUTPUT_DIR" \
            >/dev/null 2>&1 ); then
        echo "✓ $testname"
        passed=$((passed + 1))
    else
        echo "✗ $testname"
        failed=$((failed + 1))
        fails="$fails $testname"
    fi
done

echo ""
echo "Results: $passed/$total passed"
if [ $failed -gt 0 ]; then
    echo "Failed:$fails"
fi

if [ $failed -gt 0 ]; then
    exit 1
fi
exit 0
