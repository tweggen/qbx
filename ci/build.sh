#!/usr/bin/env bash
#
# ci/build.sh — build Smaragd for development, into smaragd/build/.
#
#     ./ci/build.sh                      # incremental; configures first if needed
#     ./ci/build.sh /path/to/Qt/6.x/macos
#     ./ci/build.sh --clean              # wipe smaragd/build/ and configure afresh
#     ./ci/build.sh --test               # ...then run ctest
#     ./ci/build.sh -- -DFOO=bar         # extra args for the configure step
#
# This replaces the ./build.sh and ./rebuild.sh that used to sit at the repo
# root, so every repo in the suite now answers to the same verbs: ci/build.sh,
# ci/clean.sh, ci/install.sh, ci/uninstall.sh. `--clean` is what ./rebuild.sh
# was, and a clean build is equally `./ci/clean.sh && ./ci/build.sh`.
#
# `_env.sh` DELIBERATELY STAYS AT THE REPO ROOT. It is a sourced library, not a
# script you run, and nassau-suite's own ci/build.sh reaches into this checkout
# for it (`cd qbx && source ./_env.sh`) so that there is one Qt-detection
# implementation across the product. Moving it here would break the superbuild's
# Qt, toolchain and vcpkg bootstrap in a way that surfaces only on Windows,
# several steps later, naming the wrong file (QBX-103's class of failure).
#
# THE ONE TRAP IN WIRING IT UP: `SCRIPT_DIR` in _env.sh means THE REPO ROOT, not
# "the directory this script lives in" -- it builds $SCRIPT_DIR/smaragd/... and
# $SCRIPT_DIR/.git out of it. That name was honest while the callers sat at the
# root; from ci/ it is a lie, and the rename it deserves is not available:
# nassau-suite sets SCRIPT_DIR itself before sourcing, so changing the contract
# would break a repo this change is not allowed to touch. Hence the assignment
# below, spelled out rather than copied.
#
# NO `set -u`. _env.sh is 560 lines written against `set -e` alone, and the old
# root scripts ran it that way; switching the whole library to nounset on the
# side of a layout change is a gratuitous risk, and an unset-variable abort
# inside Qt detection would read as "Qt not found".
set -eo pipefail

CI_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$CI_DIR/.." && pwd)"

# _env.sh's two inputs. SCRIPT_DIR is the repo root; see the note above.
SCRIPT_DIR="$REPO_DIR"
PROJECT_DIR="$REPO_DIR/smaragd"

CLEAN=""
RUN_TESTS=""
QT_PATH_ARG=""
EXTRA_CMAKE_ARGS=()

while [ $# -gt 0 ]; do
    case "$1" in
        --clean)     CLEAN="yes"; shift ;;
        --test)      RUN_TESTS="yes"; shift ;;
        --)          shift; EXTRA_CMAKE_ARGS=("$@"); break ;;
        -h|--help)   sed -n '2,14p' "$0"; exit 0 ;;
        -*)          echo "usage: ci/build.sh [QT_PATH] [--clean] [--test] [-- <cmake args>]" >&2
                     exit 2 ;;
        *)           # The positional Qt prefix, kept from `./build.sh [QT_PATH]`:
                     # it is what docs/BUILD.md documents, and what
                     # nassau-suite/ci/build.sh takes in the same position.
                     if [ -n "$QT_PATH_ARG" ]; then
                         echo "ci/build.sh: more than one Qt prefix given" >&2; exit 2
                     fi
                     QT_PATH_ARG="$1"; shift ;;
    esac
done

# shellcheck source=../_env.sh
source "$REPO_DIR/_env.sh"

detect_platform
set_bin_path
ensure_submodules

cd "$PROJECT_DIR"

# --- incremental, or a full configure? ----------------------------------------
# Three things force a configure, and only the first of them is a choice.
NEED_CONFIGURE=""
WHY=""

if [ -n "$CLEAN" ]; then
    NEED_CONFIGURE="yes"; WHY="--clean"
elif [ ! -d build ]; then
    NEED_CONFIGURE="yes"; WHY="no build directory yet"
fi

# A Qt upgrade under an existing build tree is NOT safe to build incrementally,
# and ninja cannot see it: dpkg preserves upstream mtimes, so the new headers can
# be OLDER than the objects compiled against the old ones. Nothing rebuilds, and
# the binary mixes old-Qt objects with the new Qt runtime -- which on 2026-09-21
# meant every qxa case SEGFAULTing in SApplication's constructor while `ctest`
# and `ninja` both reported a clean, complete build (QBX-103).
#
# Resolved only when we were otherwise going to build incrementally: the guard
# needs the resolved Qt, and resolving it is wasted work once a configure is
# already decided. The toolchain setup here is not optional either -- on Windows
# the compiler has to be on PATH for an incremental build too.
if [ -z "$NEED_CONFIGURE" ]; then
    resolve_qt_path "$QT_PATH_ARG"
    setup_toolchain
    if QT_CHANGE=$(qt_stamp_differs); then
        NEED_CONFIGURE="yes"
        WHY="Qt changed since this build tree was configured: $QT_CHANGE"
        echo "=== $WHY ==="
        echo ""
        echo "An incremental build would link objects compiled against the old Qt"
        echo "headers against the new Qt runtime. Ninja cannot detect this, because"
        echo "packaged headers keep their upstream mtime. Configuring afresh."
        echo ""
    fi
fi

if [ -n "$NEED_CONFIGURE" ]; then
    resolve_qt_path "$QT_PATH_ARG"
    setup_toolchain
    check_linux_prereqs
    pick_generator
    ensure_render_deps
    setup_extra_cmake_args

    echo "=== Smaragd build: configure + build ($WHY) ==="
    echo "Platform:    $PLATFORM"
    echo "Project dir: $PROJECT_DIR"
    echo "Qt path:     $QT_PATH"
    echo ""

    if [ -d build ]; then
        echo "Removing old build directory..."
        rm -rf build
    fi

    # AUTO_DEPLOY_QT defaults ON: a windeployqt/macdeployqt POST_BUILD step copies
    # the Qt runtime, plugins, and (MinGW) compiler runtime next to the binary so
    # it launches without Qt on PATH -- which is also what makes what
    # ci/install.sh installs runnable. Override with AUTO_DEPLOY_QT=OFF in the
    # environment for faster iteration when you run with Qt already on PATH.
    echo "Configuring CMake..."
    cmake -B build "${CMAKE_GENERATOR_ARGS[@]}" \
        -DCMAKE_PREFIX_PATH="$QT_PATH" \
        -DAUTO_DEPLOY_QT="${AUTO_DEPLOY_QT:-ON}" \
        "${CMAKE_EXTRA_ARGS[@]}" \
        "${EXTRA_CMAKE_ARGS[@]}"

    echo ""
    echo "Building..."
    cmake --build build

    # Record which Qt this tree was built against, so a later incremental build
    # can refuse to build over a Qt upgrade it cannot otherwise detect (QBX-103).
    write_qt_stamp
else
    if [ "${#EXTRA_CMAKE_ARGS[@]}" -gt 0 ]; then
        # Said, not silently dropped: those args reach the CONFIGURE step, and on
        # this path there isn't one.
        echo "ci/build.sh: ignoring the extra cmake args -- this is an incremental" >&2
        echo "             build and nothing is being configured. Use --clean." >&2
    fi
    echo "Building (incremental)..."
    cmake --build build
fi

if [ -n "$RUN_TESTS" ]; then
    echo ""
    echo "=== Test ==="
    # Serial on purpose: the suite has record and live cases that contend for the
    # audio device. ci/gates.sh is where a parallelism is chosen, and it says why.
    ctest --test-dir build --output-on-failure
fi

echo ""
echo "=== Build complete ==="
echo "Binary:  $BIN_PATH"
echo "Install: ./ci/install.sh"
