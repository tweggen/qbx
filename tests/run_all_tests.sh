#!/bin/bash
# Run all action script tests in this directory.
# Usage: ./run_all_tests.sh [path/to/smaragd] [test_dir]
# Example: ./run_all_tests.sh ../smaragd/build/bin/smaragd.app/Contents/MacOS/smaragd ./cases

SMARAGD="${1:?Usage: $0 <smaragd_binary> [test_dir]}"
TEST_DIR="${2:-.}"

if [ ! -x "$SMARAGD" ]; then
    echo "Error: $SMARAGD is not executable"
    exit 1
fi

if [ ! -d "$TEST_DIR" ]; then
    echo "Error: $TEST_DIR is not a directory"
    exit 1
fi

# Count results
passed=0
failed=0
total=0

# Run all .qxa files
for testfile in "$TEST_DIR"/*.qxa; do
    if [ ! -f "$testfile" ]; then
        continue
    fi

    total=$((total + 1))
    testname=$(basename "$testfile")

    # Run test (capture output, discard debug spew)
    if "$SMARAGD" --test-case "$testfile" 2>&1 | grep -q "^PASS"; then
        echo "✓ $testname"
        passed=$((passed + 1))
    else
        echo "✗ $testname"
        failed=$((failed + 1))
    fi
done

echo ""
echo "Results: $passed/$total passed"

if [ $failed -gt 0 ]; then
    exit 1
fi
exit 0
