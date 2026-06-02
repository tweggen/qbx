#!/bin/bash
set -e

# Incremental build script - fast rebuild for development
# Usage: ./build.sh
#
# Rebuilds only what changed. For clean rebuild, use ./rebuild.sh

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR/smaragd"

# Change to project directory
cd "$PROJECT_DIR"

# Check if build directory exists
if [ ! -d "build" ]; then
    echo "Error: build/ directory not found."
    echo "Run ./rebuild.sh first for initial setup."
    exit 1
fi

# Build (only changed files)
echo "Building (incremental)..."
cmake --build build

echo ""
echo "=== Build complete ==="
echo "Binary: $PROJECT_DIR/build/bin/smaragd.app/Contents/MacOS/smaragd"
