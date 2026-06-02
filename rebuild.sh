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
    # Try common Qt installation locations
    if [ -d "$HOME/Qt" ]; then
        # Find the most recent Qt 6 installation
        QT_PATH=$(find "$HOME/Qt" -maxdepth 3 -name "*.framework" -o -name "QtCore.framework" 2>/dev/null | \
                  head -1 | xargs dirname | xargs dirname)
    elif command -v qmake &> /dev/null; then
        QT_PATH=$(qmake -query QT_INSTALL_PREFIX)
    else
        echo "Error: Could not detect Qt installation. Please provide QT_PATH as argument."
        echo "Usage: $0 /path/to/qt"
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
