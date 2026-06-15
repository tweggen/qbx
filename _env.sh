#!/bin/bash
# Shared environment + toolchain detection for build.sh / rebuild.sh.
# This file is *sourced*, not executed. It defines helper functions and,
# after they run, these variables:
#   PLATFORM   macos | linux | windows
#   QT_PATH    Qt prefix to pass as CMAKE_PREFIX_PATH
#   BIN_PATH   path to the built binary (for the success message)
#
# On Windows (Git Bash / MSYS), the MinGW compiler and Ninja ship under
# <QtRoot>/Tools, which is NOT inside the Qt prefix. setup_toolchain()
# locates them and prepends them to PATH so CMake/Ninja find gcc/g++/ninja.
#
# Typical use from a caller:
#   SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
#   source "$SCRIPT_DIR/_env.sh"
#   detect_platform
#   resolve_qt_path "$1"
#   setup_toolchain
#   set_bin_path

detect_platform() {
    case "$(uname -s)" in
        Darwin*)              PLATFORM="macos" ;;
        Linux*)               PLATFORM="linux" ;;
        MINGW*|MSYS*|CYGWIN*) PLATFORM="windows" ;;
        *)
            echo "Warning: unknown platform '$(uname -s)', assuming linux."
            PLATFORM="linux"
            ;;
    esac
}

# $1 = optional user-provided Qt prefix. If empty, guess per platform.
resolve_qt_path() {
    if [ -n "$1" ]; then
        QT_PATH="$1"
    else
        QT_PATH=""
        case "$PLATFORM" in
            macos)
                [ -d "$HOME/Qt" ] && QT_PATH=$(ls -d "$HOME/Qt"/6.*/macos 2>/dev/null | sort -V | tail -1)
                ;;
            linux)
                [ -d "$HOME/Qt" ] && QT_PATH=$(ls -d "$HOME/Qt"/6.*/gcc_64 2>/dev/null | sort -V | tail -1)
                ;;
            windows)
                local root
                for root in /c/Qt C:/Qt "$HOME/Qt"; do
                    [ -d "$root" ] || continue
                    QT_PATH=$(ls -d "$root"/6.*/mingw_64 2>/dev/null | sort -V | tail -1)
                    [ -n "$QT_PATH" ] && break
                done
                ;;
        esac

        # Fallback: ask qmake if it's on PATH.
        if [ -z "$QT_PATH" ] && command -v qmake &>/dev/null; then
            QT_PATH=$(qmake -query QT_INSTALL_PREFIX)
        fi

        if [ -z "$QT_PATH" ]; then
            echo "Error: Could not detect Qt installation."
            echo "Usage: $0 /path/to/qt"
            case "$PLATFORM" in
                macos)   echo "Example: $0 \$HOME/Qt/6.11.1/macos" ;;
                linux)   echo "Example: $0 \$HOME/Qt/6.11.1/gcc_64" ;;
                windows) echo "Example: $0 /c/Qt/6.11.1/mingw_64" ;;
            esac
            exit 1
        fi
    fi

    # --- Normalization below runs for both user-supplied and auto-detected paths ---

    # Strip trailing slash(es) so dirname-based logic below is reliable.
    QT_PATH="${QT_PATH%/}"
    while [ "${QT_PATH%/}" != "$QT_PATH" ]; do QT_PATH="${QT_PATH%/}"; done

    # Tolerate being handed the version dir (e.g. .../Qt/6.11.1) instead of the
    # kit dir: if there's no CMake config here but a platform kit lives beneath,
    # descend into it.
    if [ ! -d "$QT_PATH/lib/cmake" ]; then
        local kit_name kit
        case "$PLATFORM" in
            macos)   kit_name="macos" ;;
            linux)   kit_name="gcc_64" ;;
            windows) kit_name="mingw_64" ;;
        esac
        kit=$(ls -d "$QT_PATH/$kit_name" 2>/dev/null | tail -1)
        if [ -n "$kit" ]; then
            echo "Note: '$QT_PATH' looks like a version dir; using kit '$kit'."
            QT_PATH="$kit"
        fi
    fi
}

# Prepend the bundled MinGW/Ninja/CMake toolchain to PATH on Windows.
# No-op on macOS/Linux, where the system compiler is already on PATH.
setup_toolchain() {
    if [ "$PLATFORM" = "windows" ]; then
        # The bundled tools live in <QtRoot>/Tools, but QT_PATH may point at the
        # kit (.../6.x.x/mingw_64), the version dir, or carry a trailing slash.
        # Walk up until we find a dir that actually contains a Tools/ subdir.
        local qt_root mingw_bin ninja_dir cmake_bin d
        qt_root=""
        d="${QT_PATH%/}"
        while [ -n "$d" ] && [ "$d" != "/" ] && [ "$d" != "." ]; do
            if [ -d "$d/Tools" ]; then qt_root="$d"; break; fi
            d=$(dirname "$d")
        done

        if [ -z "$qt_root" ]; then
            echo "Warning: could not locate a Qt 'Tools' dir above $QT_PATH."
            echo "         CMake will likely fail to find a C/C++ compiler."
            command -v ninja &>/dev/null || echo "Warning: 'ninja' not found on PATH."
            return
        fi

        mingw_bin=$(ls -d "$qt_root"/Tools/mingw*/bin 2>/dev/null | sort -V | tail -1)
        ninja_dir=$(ls -d "$qt_root"/Tools/Ninja     2>/dev/null | tail -1)
        cmake_bin=$(ls -d "$qt_root"/Tools/CMake*/bin 2>/dev/null | sort -V | tail -1)

        [ -n "$cmake_bin" ] && PATH="$cmake_bin:$PATH"
        [ -n "$ninja_dir" ] && PATH="$ninja_dir:$PATH"
        [ -n "$mingw_bin" ] && PATH="$mingw_bin:$PATH"
        export PATH

        if [ -n "$mingw_bin" ]; then
            echo "Toolchain: $mingw_bin"
        else
            echo "Warning: MinGW toolchain not found under $qt_root/Tools."
            echo "         CMake will likely fail to find a C/C++ compiler."
        fi
    fi

    command -v ninja &>/dev/null || echo "Warning: 'ninja' not found on PATH."
}

# Locate a vcpkg install (Windows). On success sets globals VCPKG_DIR (the root)
# and VCPKG_TRIPLET, and returns 0. Returns 1 if none found. Quiet — callers report.
detect_vcpkg() {
    VCPKG_DIR=""
    VCPKG_TRIPLET="${VCPKG_TARGET_TRIPLET:-x64-mingw-dynamic}"

    local root cand
    # 1) Known locations.
    for root in "$VCPKG_ROOT" "$HOME/vcpkg" /c/vcpkg /c/Users/*/vcpkg; do
        [ -n "$root" ] || continue
        [ -f "$root/scripts/buildsystems/vcpkg.cmake" ] || continue
        VCPKG_DIR="$root"
        return 0
    done

    # 2) vcpkg on PATH: the executable sits at the vcpkg root.
    cand=$(command -v vcpkg 2>/dev/null || command -v vcpkg.exe 2>/dev/null)
    if [ -n "$cand" ]; then
        root=$(dirname "$cand")
        if [ -f "$root/scripts/buildsystems/vcpkg.cmake" ]; then
            VCPKG_DIR="$root"
            return 0
        fi
    fi

    return 1
}

# Ensure the render deps (libsndfile/libvorbis + pkgconf) exist in the vcpkg
# install, installing them via vcpkg if missing. Windows only. Requires the
# MinGW toolchain already on PATH (call setup_toolchain first) so the mingw
# triplet builds against Qt's g++. The install only runs when the libs are
# absent, so it's a one-time bootstrap cost, not a per-build hit.
ensure_render_deps() {
    [ "$PLATFORM" = "windows" ] || return 0
    detect_vcpkg || { echo "Note: vcpkg not found; skipping render-dep install."; return 0; }

    local pcdir="$VCPKG_DIR/installed/$VCPKG_TRIPLET/lib/pkgconfig"
    local pkgconf_exe="$VCPKG_DIR/installed/$VCPKG_TRIPLET/tools/pkgconf/pkgconf.exe"
    # pkgconf is what satisfies find_package(PkgConfig); check it too, else a
    # libs-only install would still fail configure with "Could NOT find PkgConfig".
    if [ -f "$pcdir/sndfile.pc" ] && [ -f "$pcdir/vorbis.pc" ] && [ -f "$pkgconf_exe" ]; then
        return 0   # already installed
    fi

    echo "Render deps (libsndfile/libvorbis) missing for $VCPKG_TRIPLET."
    echo "Installing via vcpkg from $VCPKG_DIR (this can take several minutes)..."

    # Bootstrap vcpkg.exe if it isn't built yet.
    local vcpkg_exe="$VCPKG_DIR/vcpkg.exe"
    if [ ! -f "$vcpkg_exe" ]; then
        if [ -f "$VCPKG_DIR/bootstrap-vcpkg.bat" ]; then
            echo "Bootstrapping vcpkg..."
            ( cd "$VCPKG_DIR" && ./bootstrap-vcpkg.bat ) || {
                echo "Warning: vcpkg bootstrap failed; configure will likely fail."; return 0; }
        else
            echo "Warning: $VCPKG_DIR/bootstrap-vcpkg.bat not found; cannot auto-install."
            return 0
        fi
    fi

    "$vcpkg_exe" install --triplet "$VCPKG_TRIPLET" libsndfile libvorbis pkgconf || {
        echo "Warning: vcpkg install failed; configure may fail to find render deps."
        return 0
    }
    echo "Render deps installed."
}

# Populate CMAKE_EXTRA_ARGS (a bash array) with platform-specific configure
# flags. On Windows the render deps come from vcpkg's mingw triplet, so wire up
# the vcpkg toolchain file. No-op on macOS/Linux (system pkg-config + brew/apt).
setup_extra_cmake_args() {
    CMAKE_EXTRA_ARGS=()
    [ "$PLATFORM" = "windows" ] || return

    if detect_vcpkg; then
        local tc="$VCPKG_DIR/scripts/buildsystems/vcpkg.cmake"
        # CMake wants a Windows-style path (C:/...), not MSYS (/c/...).
        command -v cygpath &>/dev/null && tc="$(cygpath -m "$tc")"
        CMAKE_EXTRA_ARGS+=( "-DCMAKE_TOOLCHAIN_FILE=$tc"
                            "-DVCPKG_TARGET_TRIPLET=$VCPKG_TRIPLET" )
        echo "vcpkg: $VCPKG_DIR (triplet $VCPKG_TRIPLET)"
    else
        echo "Warning: vcpkg not found; render deps (libsndfile/libvorbis) will be missing."
        echo "         Install vcpkg + libs for x64-mingw-dynamic, or set VCPKG_ROOT."
    fi
}

set_bin_path() {
    case "$PLATFORM" in
        macos)   BIN_PATH="$PROJECT_DIR/build/bin/smaragd.app/Contents/MacOS/smaragd" ;;
        windows) BIN_PATH="$PROJECT_DIR/build/bin/smaragd.exe" ;;
        *)       BIN_PATH="$PROJECT_DIR/build/bin/smaragd" ;;
    esac
}
