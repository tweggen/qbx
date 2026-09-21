#!/bin/bash
set -e

# Incremental build script - fast rebuild for development
# Usage: ./build.sh [QT_PATH]
#
# Rebuilds only what changed. For a clean rebuild, use ./rebuild.sh.
# If build/ doesn't exist, this delegates to rebuild.sh to configure first.
# Works on macOS, Linux, and Windows (Git Bash). See _env.sh.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR/smaragd"
source "$SCRIPT_DIR/_env.sh"

detect_platform
set_bin_path
ensure_submodules

# Change to project directory
cd "$PROJECT_DIR"

# Check if build directory exists; if not, initialize it via rebuild.sh.
if [ ! -d "build" ]; then
    echo "Build directory not found. Initializing..."
    echo ""
    "$SCRIPT_DIR/rebuild.sh" "$1"
    exit 0
fi

# The compiler still needs to be on PATH for incremental builds (Windows),
# so resolve the Qt prefix and set up the toolchain even though we don't
# reconfigure here.
resolve_qt_path "$1"
setup_toolchain

# A Qt upgrade under an existing build tree is NOT safe to build incrementally,
# and ninja cannot see it: dpkg preserves upstream mtimes, so the new headers can
# be OLDER than the objects compiled against the old ones. Nothing rebuilds, and
# the binary mixes old-Qt objects with the new Qt runtime -- which on 2026-09-21
# meant every qxa case SEGFAULTing in SApplication's constructor while `ctest`
# and `ninja` both reported a clean, complete build (QBX-103).
if QT_CHANGE=$(qt_stamp_differs); then
    echo "=== Qt changed since this build tree was configured: $QT_CHANGE ==="
    echo ""
    echo "An incremental build would link objects compiled against the old Qt"
    echo "headers against the new Qt runtime. Ninja cannot detect this, because"
    echo "packaged headers keep their upstream mtime. Rebuilding cleanly."
    echo ""
    "$SCRIPT_DIR/rebuild.sh" "$1"
    exit 0
fi

# Build (only changed files)
echo "Building (incremental)..."
cmake --build build

echo ""
echo "=== Build complete ==="
echo "Binary: $BIN_PATH"
