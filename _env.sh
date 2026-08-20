#!/bin/bash
# Shared environment + toolchain detection for build.sh / rebuild.sh.
# This file is *sourced*, not executed. It defines helper functions and,
# after they run, these variables:
#   PLATFORM   macos | linux | windows
#   QT_PATH    Qt prefix to pass as CMAKE_PREFIX_PATH
#   BIN_PATH   path to the built binary (for the success message)
#   CMAKE_GENERATOR_ARGS  bash array: -G Ninja, or empty when ninja is absent
#
# Qt is found in this order: an explicit argument, the Qt installer's layout
# (~/Qt/6.x/<kit>, C:/Qt/..., Homebrew's keg), then qmake6/qtpaths6/qmake on
# PATH, then a system Qt6Config.cmake. The middle step is what finds a DISTRO
# Qt: Ubuntu's qt6-base-dev installs into /usr with its tools named qmake6 and
# qtpaths6, so a stock apt box has neither ~/Qt nor a plain "qmake".
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

# Ask a qmake/qtpaths binary for its install prefix. Echoes the prefix and
# returns 0 only for a Qt *6* installation -- a distro that still ships Qt 5 as
# plain "qmake" must not silently win over a Qt 6 next to it.
qt_prefix_from_query() {
    local exe="$1" ver prefix
    command -v "$exe" &>/dev/null || return 1
    ver=$("$exe" -query QT_VERSION 2>/dev/null) || return 1
    case "$ver" in 6.*) ;; *) return 1 ;; esac
    prefix=$("$exe" -query QT_INSTALL_PREFIX 2>/dev/null) || return 1
    [ -n "$prefix" ] || return 1
    echo "$prefix"
}

# True when a prefix actually contains a Qt 6 CMake package, whatever libdir
# layout it uses: <prefix>/lib/cmake (Qt installer, macOS/Windows),
# <prefix>/lib/<arch>/cmake (Debian/Ubuntu multiarch) or <prefix>/lib64/cmake
# (Fedora/openSUSE).
qt6_config_present() {
    local p="${1%/}" f
    [ -n "$p" ] || return 1
    # Tested one at a time on purpose: `ls a b c` returns non-zero when ANY
    # operand is missing, so a single ls over all three layouts reports "no Qt"
    # for every prefix that has exactly one of them -- i.e. for all of them.
    for f in "$p"/lib/cmake/Qt6/Qt6Config.cmake \
             "$p"/lib/*/cmake/Qt6/Qt6Config.cmake \
             "$p"/lib64/cmake/Qt6/Qt6Config.cmake; do
        [ -f "$f" ] && return 0
    done
    return 1
}

# Locate a SYSTEM Qt 6 by its CMake package dir and echo the PREFIX (which is
# what CMAKE_PREFIX_PATH wants). A distro package -- Ubuntu/Debian
# qt6-base-dev, Fedora qt6-qtbase-devel -- installs into
# <prefix>/lib/<arch>/cmake/Qt6, which is neither ~/Qt nor a "kit" directory,
# so none of the Qt-installer-shaped guesses can see it. This is the last
# resort: it runs only when no qmake6/qtpaths6 is on PATH (qt6-base-dev-tools
# not installed, or a stripped container).
detect_system_qt6_prefix() {
    local cfg dir libdir
    for cfg in /usr/lib/*/cmake/Qt6/Qt6Config.cmake \
               /usr/lib/cmake/Qt6/Qt6Config.cmake \
               /usr/lib64/cmake/Qt6/Qt6Config.cmake \
               /usr/local/lib/*/cmake/Qt6/Qt6Config.cmake \
               /usr/local/lib/cmake/Qt6/Qt6Config.cmake \
               /usr/local/lib64/cmake/Qt6/Qt6Config.cmake \
               /opt/qt6*/lib/*/cmake/Qt6/Qt6Config.cmake \
               /opt/qt6*/lib/cmake/Qt6/Qt6Config.cmake; do
        [ -f "$cfg" ] || continue
        # <prefix>/lib[/<arch>]/cmake/Qt6/Qt6Config.cmake -> <prefix>
        dir=$(dirname "$cfg")                  # .../cmake/Qt6
        dir=$(dirname "$(dirname "$dir")")     # .../lib or .../lib/<arch>
        libdir=$(basename "$dir")
        if [ "$libdir" != "lib" ] && [ "$libdir" != "lib64" ]; then
            dir=$(dirname "$dir")              # strip the multiarch component
        fi
        dirname "$dir"
        return 0
    done
    return 1
}

# $1 = optional user-provided Qt prefix. If empty, guess per platform.
resolve_qt_path() {
    if [ -n "$1" ]; then
        QT_PATH="$1"
    else
        QT_PATH=""
        local exe
        case "$PLATFORM" in
            macos)
                [ -d "$HOME/Qt" ] && QT_PATH=$(ls -d "$HOME/Qt"/6.*/macos 2>/dev/null | sort -V | tail -1)
                # Homebrew's qt keg (not symlinked into /usr/local by default).
                if [ -z "$QT_PATH" ]; then
                    for exe in /opt/homebrew/opt/qt /usr/local/opt/qt \
                               /opt/homebrew/opt/qt6 /usr/local/opt/qt6; do
                        if qt6_config_present "$exe"; then QT_PATH="$exe"; break; fi
                    done
                fi
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

        # Fallback 1: ask a Qt tool on PATH. A DISTRO Qt 6 names these
        # qmake6/qtpaths6 (Ubuntu qt6-base-dev-tools, Fedora qt6-qtbase-devel)
        # and never plain "qmake" -- which is the entire reason a stock Ubuntu
        # box used to end up in the "Could not detect Qt" branch below.
        if [ -z "$QT_PATH" ]; then
            for exe in qmake6 qtpaths6 qmake qtpaths; do
                QT_PATH=$(qt_prefix_from_query "$exe" || true)
                if [ -n "$QT_PATH" ]; then
                    echo "Qt: detected via $exe -> $QT_PATH"
                    break
                fi
            done
        fi

        # Fallback 2: the CMake package dir itself, for a Qt installed without
        # its dev tools on PATH.
        if [ -z "$QT_PATH" ]; then
            QT_PATH=$(detect_system_qt6_prefix || true)
            [ -n "$QT_PATH" ] && echo "Qt: detected via CMake package dir -> $QT_PATH"
        fi

        if [ -z "$QT_PATH" ]; then
            echo "Error: Could not detect Qt installation."
            echo "Usage: $0 /path/to/qt"
            case "$PLATFORM" in
                macos)   echo "Example: $0 \$HOME/Qt/6.11.1/macos" ;;
                linux)
                    echo "Example: $0 \$HOME/Qt/6.11.1/gcc_64"
                    echo ""
                    echo "On Debian/Ubuntu the distro Qt 6 is enough; install it with:"
                    echo "  sudo apt install $(apt_packages_line)"
                    ;;
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
    # kit dir: if there's no Qt 6 package here but a platform kit lives beneath,
    # descend into it. The test is for a Qt6Config.cmake rather than for a
    # lib/cmake DIRECTORY, because a distro prefix is /usr -- where lib/cmake
    # may not exist at all (Ubuntu puts it under lib/<arch>/cmake) and where
    # descending into a "kit" would be nonsense.
    if ! qt6_config_present "$QT_PATH"; then
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

    if ! qt6_config_present "$QT_PATH"; then
        echo "Warning: no Qt6Config.cmake found under '$QT_PATH'."
        echo "         CMake will probably fail with \"Could not find a package configuration file"
        echo "         provided by Qt6\"."
        if [ "$PLATFORM" = "linux" ]; then
            echo "         On Debian/Ubuntu: sudo apt install $(apt_packages_line)"
        fi
    fi
}

# The Debian/Ubuntu package list this build needs, as one apt-installable line.
# Kept in ONE place because it is quoted from three: the "could not detect Qt"
# error, the "no Qt6Config.cmake" warning, and check_linux_prereqs below.
#
# Nothing here is optional: qt6-base-dev is Core/Gui/Widgets/Xml/Network/
# Concurrent AND it depends on the `qmake6` package, which is what puts
# qmake6/qtpaths6 on PATH -- i.e. how this script finds Qt at all. (It is
# `qmake6`, NOT `qt6-base-dev-tools`; that one ships androiddeployqt6 and
# friends and has nothing to do with detection.) libsndfile1-dev /
# libogg-dev / libvorbis-dev / libasound2-dev are REQUIRED find_package and
# pkg_check_modules calls in tw303a/CMakeLists.txt.
apt_packages_line() {
    echo "build-essential cmake ninja-build pkg-config git qt6-base-dev \
libsndfile1-dev libogg-dev libvorbis-dev libasound2-dev"
}

# Report Linux build prerequisites that are missing, as an apt line the user can
# paste. A WARNING, not a hard stop: a hand-built libsndfile with no .pc file is
# a legitimate setup this heuristic cannot see, and CMake gives the real error
# either way. Call it AFTER resolve_qt_path so QT_PATH can be checked too.
check_linux_prereqs() {
    [ "$PLATFORM" = "linux" ] || return 0

    local missing=() notes=() line
    command -v cmake &>/dev/null       || missing+=("cmake")
    command -v g++   &>/dev/null       || missing+=("build-essential")
    command -v git   &>/dev/null       || missing+=("git")
    command -v ninja &>/dev/null       || missing+=("ninja-build")
    command -v pkg-config &>/dev/null  || missing+=("pkg-config")

    qt6_config_present "${QT_PATH:-}" || missing+=("qt6-base-dev")
    command -v qmake6 &>/dev/null || command -v qtpaths6 &>/dev/null \
        || missing+=("qmake6")

    # The render/analysis deps are pkg-config modules on Linux (tw303a/CMakeLists.txt).
    if command -v pkg-config &>/dev/null; then
        pkg-config --exists sndfile   || missing+=("libsndfile1-dev")
        pkg-config --exists ogg       || missing+=("libogg-dev")
        pkg-config --exists vorbis vorbisenc || missing+=("libvorbis-dev")
        pkg-config --exists alsa      || missing+=("libasound2-dev")
        pkg-config --exists libsecret-1 \
            || notes+=("libsecret-1-dev is not installed (optional: the libsecret credential store backend).")
    else
        # Without pkg-config the four render/audio deps cannot be probed at all,
        # so name them rather than silently reporting a shorter list than the
        # one the user actually needs.
        notes+=("pkg-config is missing, so libsndfile1-dev / libogg-dev / libvorbis-dev / libasound2-dev could not be checked -- install them alongside it.")
    fi

    if [ ${#missing[@]} -gt 0 ]; then
        # De-duplicate while keeping order.
        local uniq=() m u seen
        for m in "${missing[@]}"; do
            seen=""
            for u in "${uniq[@]:-}"; do [ "$u" = "$m" ] && seen=1; done
            [ -n "$seen" ] || uniq+=("$m")
        done
        echo ""
        echo "Warning: these Debian/Ubuntu packages appear to be missing:"
        echo "  sudo apt install ${uniq[*]}"
        echo "         (CMake will fail below if they really are absent.)"
    fi
    for line in "${notes[@]:-}"; do
        [ -n "$line" ] && echo "Note: $line"
    done
    return 0
}

# Populate CMAKE_GENERATOR_ARGS. Ninja is the project's generator everywhere and
# is bundled with the Qt installer on Windows, but on a distro it is a separate
# package (ninja-build) that a Qt install does NOT pull in. Falling back to the
# default generator keeps a box with qt6-base-dev but no ninja-build building,
# instead of failing configure with "CMAKE_MAKE_PROGRAM is not set".
pick_generator() {
    CMAKE_GENERATOR_ARGS=()
    if command -v ninja &>/dev/null; then
        CMAKE_GENERATOR_ARGS=( -G Ninja )
    else
        echo "Note: 'ninja' not found; using CMake's default generator."
        [ "$PLATFORM" = "linux" ] && echo "      For faster builds: sudo apt install ninja-build"
    fi
    return 0
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

# Check out the git submodules under smaragd/third_party/ if they are missing.
#
# The plugin SDKs (CLAP, VST3 interfaces — proposal 08) are header-only vendored
# dependencies with no package-manager path on ANY platform, so a clone made
# without --recurse-submodules would silently configure without plugin hosting.
# The sentinel is a real header, not the directory: an uninitialised submodule
# leaves an empty directory behind, which "[ -d ]" would happily accept.
#
# Called from BOTH build.sh and rebuild.sh on purpose: build.sh deliberately
# skips ensure_render_deps, so a dependency hook placed only in rebuild.sh would
# never run on the common incremental path.
ensure_submodules() {
    local sentinel="$SCRIPT_DIR/smaragd/third_party/clap/include/clap/clap.h"
    [ -f "$sentinel" ] && return 0

    if [ ! -d "$SCRIPT_DIR/.git" ] && [ ! -f "$SCRIPT_DIR/.git" ]; then
        echo "Warning: $SCRIPT_DIR is not a git checkout; cannot fetch third_party submodules."
        echo "         Plugin hosting (CLAP/VST3) will be disabled in this build."
        return 0
    fi
    if ! command -v git &>/dev/null; then
        echo "Warning: 'git' not found on PATH; cannot fetch third_party submodules."
        echo "         Plugin hosting (CLAP/VST3) will be disabled in this build."
        return 0
    fi

    echo "Third-party submodules missing; running 'git submodule update --init --recursive'..."
    ( cd "$SCRIPT_DIR" && git submodule update --init --recursive ) || {
        echo "Warning: submodule checkout failed (offline?)."
        echo "         Plugin hosting (CLAP/VST3) will be disabled in this build."
        return 0
    }

    if [ -f "$sentinel" ]; then
        echo "Third-party submodules ready."
        # The SDK discovery lives in tw303a/CMakeLists.txt, and CMake only
        # re-runs when one of its inputs changes. Submodule content appearing is
        # invisible to that dependency graph, so on the incremental path
        # (build.sh, existing build/) TW_HAVE_CLAP would stay off until the next
        # unrelated CMake edit. Touching the file that does the discovery makes
        # Ninja reconfigure exactly once.
        touch "$SCRIPT_DIR/smaragd/tw303a/CMakeLists.txt" 2>/dev/null || true
    else
        echo "Warning: submodule checkout reported success but $sentinel is still absent."
    fi
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

# Ensure the render deps (libsndfile/libvorbis, which pulls libogg) exist in
# the vcpkg install, installing them via vcpkg if missing. Windows only.
# Requires the MinGW toolchain already on PATH (call setup_toolchain first) so
# the mingw triplet builds against Qt's g++. The install only runs when the
# libs are absent, so it's a one-time bootstrap cost, not a per-build hit.
# (Rubber Band was removed 2026-07-26 — the in-house vocoder is the synthesis
# engine; no vcpkg overlay needed anymore.)
ensure_render_deps() {
    [ "$PLATFORM" = "windows" ] || return 0
    detect_vcpkg || { echo "Note: vcpkg not found; skipping render-dep install."; return 0; }

    # libsndfile/libvorbis ship CMake config packages (keyed off *Config.cmake).
    local sharedir="$VCPKG_DIR/installed/$VCPKG_TRIPLET/share"
    if [ -f "$sharedir/SndFile/SndFileConfig.cmake" ] && \
       [ -f "$sharedir/Vorbis/VorbisConfig.cmake" ]; then
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

    # --host-triplet is essential: without it vcpkg builds its host/build-time
    # tools for the default host triplet (x64-windows = MSVC), which fails with
    # "Unable to find a valid Visual Studio instance" on a MinGW-only machine.
    # Pinning host == target lets it build everything with Qt's g++ (on PATH).
    "$vcpkg_exe" install \
        --triplet "$VCPKG_TRIPLET" --host-triplet "$VCPKG_TRIPLET" \
        libsndfile libvorbis || {
        echo "Warning: vcpkg install failed; configure may fail to find render deps."
        return 0
    }
    echo "Render/synthesis deps installed."
}

# Populate CMAKE_EXTRA_ARGS (a bash array) with platform-specific configure
# flags. On Windows the render deps come from vcpkg's mingw triplet, so wire up
# the vcpkg toolchain file. No-op on macOS/Linux (system pkg-config + brew/apt).
setup_extra_cmake_args() {
    CMAKE_EXTRA_ARGS=()
    [ "$PLATFORM" = "windows" ] || return 0

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
    return 0
}

set_bin_path() {
    case "$PLATFORM" in
        macos)   BIN_PATH="$PROJECT_DIR/build/bin/smaragd.app/Contents/MacOS/smaragd" ;;
        windows) BIN_PATH="$PROJECT_DIR/build/bin/smaragd.exe" ;;
        *)       BIN_PATH="$PROJECT_DIR/build/bin/smaragd" ;;
    esac
}
