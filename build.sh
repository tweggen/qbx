#!/bin/bash
set -e

# Incremental build script - fast rebuild for development
# Usage: ./build.sh [QT_PATH]
#
# Rebuilds only what changed. For clean rebuild, use ./rebuild.sh
# If build/ doesn't exist, automatically initializes it first.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR/smaragd"

# Change to project directory
cd "$PROJECT_DIR"

# Check if build directory exists; if not, initialize it
if [ ! -d "build" ]; then
    echo "Build directory not found. Initializing..."
    echo ""

    # Run rebuild.sh to initialize (pass QT_PATH if provided)
    if [ -z "$1" ]; then
        "$SCRIPT_DIR/rebuild.sh"
    else
        "$SCRIPT_DIR/rebuild.sh" "$1"
    fi
    exit 0
fi

# Build (only changed files)
echo "Building (incremental)..."
cmake --build build

echo ""
echo "=== Build complete ==="
echo "Binary: $PROJECT_DIR/build/bin/smaragd.app/Contents/MacOS/smaragd"
