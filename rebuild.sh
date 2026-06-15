#!/bin/bash
set -e

# Clean rebuild script - platform agnostic with relative paths
# Usage: ./rebuild.sh [QT_PATH]
#
# Works on macOS, Linux, and Windows (Git Bash). If QT_PATH is not given,
# it is guessed per platform. On Windows the bundled MinGW/Ninja toolchain
# is located and added to PATH automatically. See _env.sh.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR/smaragd"
source "$SCRIPT_DIR/_env.sh"

detect_platform
resolve_qt_path "$1"
setup_toolchain
setup_extra_cmake_args
set_bin_path

echo "=== Smaragd Clean Rebuild ==="
echo "Platform:    $PLATFORM"
echo "Project dir: $PROJECT_DIR"
echo "Qt path:     $QT_PATH"
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
    -DAUTO_DEPLOY_QT=OFF \
    "${CMAKE_EXTRA_ARGS[@]}"

# Build
echo ""
echo "Building..."
cmake --build build

echo ""
echo "=== Build complete ==="
echo "Binary: $BIN_PATH"
