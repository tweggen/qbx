#!/bin/bash
set -e

# Clean rebuild script - platform agnostic with relative paths
# Usage: ./rebuild.sh [QT_PATH]
#
# If QT_PATH not provided, tries to detect it or use sensible defaults.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR/smaragd"

# Detect Qt path if not provided
if [ -z "$1" ]; then
    QT_PATH=""

    # Try macOS Qt installation (most recent version)
    if [ -d "$HOME/Qt" ]; then
        # Look for Qt6 macOS installation: ~/Qt/6.x.x/macos
        QT_PATH=$(ls -d "$HOME/Qt"/6.*/macos 2>/dev/null | sort -V | tail -1)
    fi

    # Fallback: try qmake if available
    if [ -z "$QT_PATH" ] && command -v qmake &> /dev/null; then
        QT_PATH=$(qmake -query QT_INSTALL_PREFIX)
    fi

    # If still not found, error
    if [ -z "$QT_PATH" ]; then
        echo "Error: Could not detect Qt installation."
        echo "Looked in: $HOME/Qt/6.*/macos"
        echo ""
        echo "Usage: $0 /path/to/qt"
        echo "Example: $0 $HOME/Qt/6.11.1/macos"
        exit 1
    fi
else
    QT_PATH="$1"
fi

echo "=== Smaragd Clean Rebuild ==="
echo "Project dir: $PROJECT_DIR"
echo "Qt path: $QT_PATH"
echo ""

# Change to project directory
cd "$PROJECT_DIR"

# Remove old build
if [ -d "build" ]; then
    echo "Removing old build directory..."
    rm -rf build
fi

# Configure
echo "Configuring CMake..."
cmake -B build -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_PATH" \
    -DAUTO_DEPLOY_QT=OFF

# Build
echo ""
echo "Building..."
cmake --build build

echo ""
echo "=== Build complete ==="
echo "Binary: $PROJECT_DIR/build/bin/smaragd.app/Contents/MacOS/smaragd"
