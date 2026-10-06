#!/usr/bin/env bash
#
# ci/install.sh — install the development build of Smaragd for the current user.
#
#     ./ci/install.sh              # into the per-user location, no password
#     ./ci/install.sh --link       # symlink instead of copy (macOS/Linux)
#     ./ci/install.sh --system     # into the shared location the .pkg writes
#     ./ci/install.sh --uninstall  # remove what this would install
#     ./ci/install.sh -n           # print what would happen, change nothing
#
# ci/uninstall.sh is this script with --uninstall, and is a forwarder rather than
# a second script ON PURPOSE: an uninstaller that does not remove exactly what
# the installer wrote is worse than none, and two files are two chances for the
# destination table below to disagree with itself.
#
# WHERE THINGS GO, and why not where the release package puts them:
#
#                 --user (default)                    --system
#   macOS         ~/Applications                      /Applications
#   Windows       %LOCALAPPDATA%\Programs\Smaragd     %PROGRAMFILES%\Smaragd
#   Linux         ~/.local/{bin,lib}                  /usr/local/{bin,lib}
#
# The per-user location needs no administrator password, which is the whole
# point for something you reinstall twenty times a day, and it leaves the copy a
# real `ci/package.sh` installer put in the system location alone. nassau-suite's
# README documents the same two domains for the shipped .pkg.
#
# THIS IS NOT A PACKAGE. It installs the build tree's output as-is: unsigned,
# unnotarized, and on macOS carrying whatever Qt the build deployed into the
# bundle. ci/package.sh in nassau-suite is what makes something shippable.
set -euo pipefail

CI_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$CI_DIR/.." && pwd)"
BIN_DIR="$REPO_DIR/smaragd/build/bin"

DOMAIN="user"
METHOD="copy"
MODE="install"
DRY=""

while [ $# -gt 0 ]; do
    case "$1" in
        --user)      DOMAIN="user"; shift ;;
        --system)    DOMAIN="system"; shift ;;
        --link)      METHOD="link"; shift ;;
        --copy)      METHOD="copy"; shift ;;
        --uninstall) MODE="uninstall"; shift ;;
        -n|--dry-run) DRY="yes"; shift ;;
        -h|--help)   sed -n '2,10p' "$0"; exit 0 ;;
        *) echo "usage: ci/install.sh [--user|--system] [--link] [--uninstall] [-n]" >&2
           exit 2 ;;
    esac
done

say()  { printf '\n=== %s ===\n' "$*"; }
note() { printf '  %s\n' "$*"; }
warn() { printf '  WARNING: %s\n' "$*"; }

# Every mutation goes through this, so -n cannot drift from the real run: there
# is one code path and one decision about whether to execute it.
SUDO=""
run() {
    if [ -n "$DRY" ]; then
        printf '  would run: %s%s\n' "${SUDO:+sudo }" "$*"
    else
        ${SUDO:+sudo} "$@"
    fi
}

case "$(uname -s)" in
    Darwin)                  PLATFORM="macos" ;;
    Linux)                   PLATFORM="linux" ;;
    MINGW*|MSYS*|CYGWIN*)    PLATFORM="windows" ;;
    *) echo "ci/install.sh: unsupported platform $(uname -s)" >&2; exit 2 ;;
esac

# --- the destination table ----------------------------------------------------
# One place, read by both install and uninstall.
case "$PLATFORM:$DOMAIN" in
    macos:user)     DEST="$HOME/Applications" ;;
    macos:system)   DEST="/Applications" ;;
    windows:user)   DEST="${LOCALAPPDATA:-$HOME/AppData/Local}/Programs/Smaragd" ;;
    windows:system) DEST="${PROGRAMFILES:-/c/Program Files}/Smaragd" ;;
    linux:user)     DEST="$HOME/.local" ;;
    linux:system)   DEST="/usr/local" ;;
esac

# The other domain, so we can report a copy that will shadow this one.
case "$PLATFORM:$DOMAIN" in
    macos:user)     OTHER="/Applications" ;;
    macos:system)   OTHER="$HOME/Applications" ;;
    windows:user)   OTHER="${PROGRAMFILES:-/c/Program Files}/Smaragd" ;;
    windows:system) OTHER="${LOCALAPPDATA:-$HOME/AppData/Local}/Programs/Smaragd" ;;
    linux:user)     OTHER="/usr/local" ;;
    linux:system)   OTHER="$HOME/.local" ;;
esac

# Writability decides whether sudo is needed, rather than assuming --system does
# and --user does not: an admin user owns /Applications on a stock macOS box and
# needs no password there, while a home directory on a locked-down host can be
# the awkward one.
need_sudo_for() {
    local target="$1" probe="$1"
    while [ -n "$probe" ] && [ ! -e "$probe" ]; do probe="$(dirname "$probe")"; done
    [ -w "$probe" ] && return 1 || return 0
}

# =============================================================================
# uninstall
# =============================================================================
if [ "$MODE" = "uninstall" ]; then
    say "Uninstall ($DOMAIN domain)"
    VICTIMS=()
    case "$PLATFORM" in
        macos)   [ -e "$DEST/smaragd.app" ] && VICTIMS+=("$DEST/smaragd.app") ;;
        windows) [ -e "$DEST" ] && VICTIMS+=("$DEST") ;;
        linux)
            [ -e "$DEST/lib/smaragd" ] && VICTIMS+=("$DEST/lib/smaragd")
            [ -e "$DEST/bin/smaragd" ] && VICTIMS+=("$DEST/bin/smaragd")
            ;;
    esac

    if [ "${#VICTIMS[@]}" -eq 0 ]; then
        note "nothing installed in the $DOMAIN domain"
        case "$PLATFORM" in
            macos)   note "looked for: $DEST/smaragd.app" ;;
            windows) note "looked for: $DEST" ;;
            linux)   note "looked for: $DEST/lib/smaragd and $DEST/bin/smaragd" ;;
        esac
        # Not an error: "already absent" is the state uninstall exists to reach.
        exit 0
    fi

    for v in "${VICTIMS[@]}"; do
        need_sudo_for "$v" && SUDO="sudo" || SUDO=""
        printf '  %s%s\n' "$(readlink "$v" >/dev/null 2>&1 && echo 'unlink ' || echo 'remove ')" "$v"
        run rm -rf -- "$v"
    done
    SUDO=""

    if [ -n "$DRY" ]; then
        say "Dry run"; note "Nothing was removed."
    else
        say "Done"
    fi
    exit 0
fi

# =============================================================================
# install
# =============================================================================
say "Install ($DOMAIN domain, $METHOD)"

if [ ! -d "$BIN_DIR" ]; then
    echo "  FAIL: no build output at $BIN_DIR" >&2
    echo "  Run ./ci/build.sh first." >&2
    exit 1
fi

if [ "$METHOD" = "link" ] && [ "$PLATFORM" = "windows" ]; then
    # Said rather than attempted: a symlink on Windows needs Developer Mode or
    # elevation, and the failure is a permission error that names neither.
    warn "--link needs Developer Mode or elevation on Windows; copying instead."
    METHOD="copy"
fi

note "from $BIN_DIR"
note "to   $DEST"

case "$PLATFORM" in
macos)
    APP=""
    # *.app ONLY. bin/ also holds the qxa plugin test fixtures (twtestclap.clap,
    # twtestvst3*.vst3) and every test executable; none of that is product.
    for a in "$BIN_DIR"/*.app; do [ -d "$a" ] && APP="$a"; done
    if [ -z "$APP" ]; then
        echo "  FAIL: no .app bundle in $BIN_DIR — build first." >&2
        exit 1
    fi
    TARGET="$DEST/$(basename "$APP")"

    # MEASURED, not assumed: a bundle built with AUTO_DEPLOY_QT=OFF carries no Qt
    # and will not launch from the Finder, where nothing puts Qt on the dynamic
    # loader's path. It runs fine from the shell that built it, so this is
    # exactly the kind of difference that costs an hour to understand.
    if [ ! -d "$APP/Contents/Frameworks/QtCore.framework" ]; then
        warn "this bundle has no Qt inside it (built with AUTO_DEPLOY_QT=OFF)."
        warn "It will run from a shell that has Qt on DYLD_FRAMEWORK_PATH and"
        warn "fail to launch from the Finder. Rebuild without that override to"
        warn "get a bundle that stands alone."
    fi

    need_sudo_for "$DEST" && SUDO="sudo" || SUDO=""
    run mkdir -p "$DEST"
    run rm -rf -- "$TARGET"
    if [ "$METHOD" = "link" ]; then
        run ln -s "$APP" "$TARGET"
    else
        # -R, and cp rather than rsync: rsync is not on a stock macOS 26 box.
        run cp -R "$APP" "$TARGET"
    fi
    INSTALLED="$TARGET"
    ;;

windows)
    EXE="$BIN_DIR/smaragd.exe"
    if [ ! -f "$EXE" ]; then
        echo "  FAIL: no smaragd.exe in $BIN_DIR — build first." >&2
        exit 1
    fi
    # The deliverable here is NOT one entry a glob can name: windeployqt puts the
    # Qt runtime BESIDE the exe, so it is the exe plus its DLLs plus platforms/
    # and styles/. Staging this wrong is what left nassau-suite's stage with no
    # DAW in it at all (QBX-135), and the same shape applies to installing it.
    need_sudo_for "$DEST" && SUDO="sudo" || SUDO=""
    run mkdir -p "$DEST"
    run cp "$EXE" "$DEST/"
    for dll in "$BIN_DIR"/*.dll; do
        [ -f "$dll" ] && run cp "$dll" "$DEST/"
    done
    for d in platforms styles; do
        if [ -d "$BIN_DIR/$d" ]; then
            run rm -rf -- "$DEST/$d"
            run cp -R "$BIN_DIR/$d" "$DEST/$d"
        else
            warn "$BIN_DIR/$d is missing; the app cannot start without platforms/."
        fi
    done
    INSTALLED="$DEST/smaragd.exe"
    ;;

linux)
    EXE="$BIN_DIR/smaragd"
    if [ ! -f "$EXE" ]; then
        echo "  FAIL: no smaragd binary in $BIN_DIR — build first." >&2
        exit 1
    fi

    # There is no macdeployqt/windeployqt equivalent in this build on Linux, so
    # the binary keeps depending on the Qt it was LINKED against. Read that prefix
    # out of the build tree rather than re-resolving it: what the build used and
    # what detection would pick today are not the same question, and only the
    # first one can make this binary start.
    QT_PREFIX=""
    CACHE="$REPO_DIR/smaragd/build/CMakeCache.txt"
    if [ -f "$CACHE" ]; then
        # First entry only: CMAKE_PREFIX_PATH is a ;-separated list, and Qt is
        # what ci/build.sh puts at the front of it.
        QT_PREFIX="$(sed -n 's/^CMAKE_PREFIX_PATH:[A-Z]*=//p' "$CACHE" \
                     | head -1 | cut -d';' -f1)"
    fi

    need_sudo_for "$DEST" && SUDO="sudo" || SUDO=""
    run mkdir -p "$DEST/lib/smaragd" "$DEST/bin"
    run rm -rf -- "$DEST/lib/smaragd/smaragd"
    if [ "$METHOD" = "link" ]; then
        run ln -s "$EXE" "$DEST/lib/smaragd/smaragd"
    else
        run cp "$EXE" "$DEST/lib/smaragd/smaragd"
    fi

    # A launcher, always, so that $DEST/bin/smaragd means the same thing whether
    # or not Qt happens to live somewhere the loader already searches.
    LAUNCHER="$(mktemp)"
    {
        echo '#!/bin/sh'
        echo '# Generated by qbx ci/install.sh — do not edit; re-run the installer.'
        case "$QT_PREFIX" in
            ''|/usr|/usr/local)
                echo '# Qt came from a system prefix, so the loader finds it unaided.' ;;
            *)
                printf '# Qt lives outside the loader default search path (%s).\n' "$QT_PREFIX"
                printf 'LD_LIBRARY_PATH="%s/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"\n' "$QT_PREFIX"
                echo 'export LD_LIBRARY_PATH' ;;
        esac
        printf 'exec "%s/lib/smaragd/smaragd" "$@"\n' "$DEST"
    } > "$LAUNCHER"
    chmod 0755 "$LAUNCHER"
    run cp "$LAUNCHER" "$DEST/bin/smaragd"
    rm -f "$LAUNCHER"
    INSTALLED="$DEST/bin/smaragd"

    case "$QT_PREFIX" in
        ''|/usr|/usr/local)
            note "Qt: system prefix (${QT_PREFIX:-unrecorded}) — no library path needed" ;;
        *)  note "Qt: $QT_PREFIX — the launcher adds it to LD_LIBRARY_PATH" ;;
    esac
    case ":${PATH}:" in
        *":$DEST/bin:"*) : ;;
        *) warn "$DEST/bin is not on your PATH, so \`smaragd\` will not resolve." ;;
    esac
    ;;
esac
SUDO=""

# --- shadowing ----------------------------------------------------------------
# Two copies in two domains is the one failure mode this kind of install creates,
# and it is silent: which one a double-click or a `smaragd` on PATH reaches is
# then down to LaunchServices or PATH order, not to which one you just built.
SHADOW=""
case "$PLATFORM" in
    macos)   [ -e "$OTHER/smaragd.app" ] && SHADOW="$OTHER/smaragd.app" ;;
    windows) [ -e "$OTHER/smaragd.exe" ] && SHADOW="$OTHER/smaragd.exe" ;;
    linux)   [ -e "$OTHER/bin/smaragd" ] && SHADOW="$OTHER/bin/smaragd" ;;
esac
if [ -n "$SHADOW" ]; then
    say "Another copy is installed"
    warn "$SHADOW"
    warn "Two installed copies of Smaragd now exist in different domains, and"
    warn "which one opens is not decided by which one you just built. Remove the"
    warn "other with: ./ci/uninstall.sh $([ "$DOMAIN" = user ] && echo --system || echo --user)"
fi

if [ -n "$DRY" ]; then
    say "Dry run"
    note "Nothing was installed."
else
    say "Installed"
    note "$INSTALLED"
    note "Remove it with: ./ci/uninstall.sh$([ "$DOMAIN" = system ] && echo ' --system' || true)"
fi
