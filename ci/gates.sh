#!/usr/bin/env bash
#
# ci/gates.sh — the gate from CLAUDE.md's "Gates before every PR", in one
# command, so that the checklist cannot drift from what CI runs.
#
#     ./ci/gates.sh --static   # the four static checkers only (seconds, no deps)
#     ./ci/gates.sh            # build + the four checkers + ctest
#
# The logic lives here and not in a workflow file (plan 50, D6): a move off
# GitHub should rewrite the YAML that CALLS this, not re-implement the gate.
# And it is the same command a developer runs, so "it passed on my machine" and
# "it passed in CI" mean the same thing — which is the whole point, given that
# until now this checklist was something a human had to remember.
#
# --static needs nothing but python3: no Qt, no compiler, no submodules. That
# makes it the cheap job that can be required on every push, while the full
# gate costs a Qt build.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_DIR"

STATIC_ONLY=""
[ "${1:-}" = "--static" ] && STATIC_ONLY=1 && shift
if [ $# -gt 0 ]; then
    echo "usage: ci/gates.sh [--static]" >&2; exit 2
fi

say() { printf '\n=== %s ===\n' "$*"; }

# The four checkers, in CLAUDE.md's order. Stdlib-only python, so this runs
# anywhere python3 does.
run_static() {
    say "Static checkers"
    local t
    for t in check_layering check_logging check_includes check_tempo_authority; do
        printf '  %-24s' "$t"
        if python3 "tools/$t.py" >"/tmp/$t.log" 2>&1; then
            echo "PASS"
        else
            echo "FAIL"
            sed 's/^/      /' "/tmp/$t.log"
            return 1
        fi
    done
}

if [ -n "$STATIC_ONLY" ]; then
    run_static
    say "Pass (static only)"
    exit 0
fi

# build.sh resolves Qt itself (_env.sh: a ~/Qt tree, a Homebrew keg, or a
# qmake6/qtpaths6 on PATH, which is how a distro Qt is found). Nothing here
# second-guesses it — one Qt-detection implementation in the product.
say "Build"
./build.sh

run_static

# CLAUDE.md: "scale -j to the machine". 406 cases, ~475 s serially on an
# Apple-silicon box, ~368 s at -j4 on a GitHub runner.
#
# CTEST_EXCLUDE is a ctest -E regex, and it is EMPTY BY DEFAULT on purpose: a
# local run should exclude nothing. CI sets it for the cases its environment
# cannot host, and every such case has an entry in the platform gate doc
# (docs/LINUX_GATE.md, docs/MACOS_GATE.md, docs/ASIO_WINDOWS_GATE.md) saying
# why. An exclusion with no entry there is a bug, not a configuration.
say "ctest"
CTEST_ARGS=( --test-dir smaragd/build -j"${CTEST_JOBS:-4}" --output-on-failure )
if [ -n "${CTEST_EXCLUDE:-}" ]; then
    CTEST_ARGS+=( -E "$CTEST_EXCLUDE" )
    echo "  excluding: $CTEST_EXCLUDE   (see the platform gate doc for why)"
fi
ctest "${CTEST_ARGS[@]}"

say "Pass"
