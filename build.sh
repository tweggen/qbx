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

# Build (only changed files)
echo "Building (incremental)..."
cmake --build build

echo ""
echo "=== Build complete ==="
echo "Binary: $BIN_PATH"
