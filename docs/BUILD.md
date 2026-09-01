# Building smaragd

The build system is **CMake** (>= 3.16).

All CMake commands below should be run from the `smaragd/` subdirectory of the
repository (the directory that contains `CMakeLists.txt`).

## Quick start (recommended): the build scripts

Two scripts at the repo root wrap the CMake invocation and work on macOS,
Linux, and Windows (Git Bash / MSYS). They detect the platform and locate the
toolchain for you:

```bash
./rebuild.sh [QT_PATH]   # clean rebuild (wipes build/, configures, builds)
./build.sh   [QT_PATH]   # incremental build (configures first if build/ is missing)
```

`QT_PATH` is the Qt prefix (e.g. `/c/Qt/6.11.1/mingw_64`,
`$HOME/Qt/6.11.1/macos`, `$HOME/Qt/6.11.1/gcc_64`, or `/usr` for a distro Qt).
If omitted it is detected, in this order: the Qt installer's layout
(`~/Qt/6.x/<kit>`, `C:/Qt/...`, Homebrew's keg), then `qmake6`/`qtpaths6`/
`qmake` on PATH, then a system `Qt6Config.cmake` under `/usr`, `/usr/local` or
`/opt/qt6*`. You can also point at any kit explicitly.

What the scripts handle automatically (logic lives in `_env.sh`, sourced by
both):

- **Platform detection** via `uname`.
- **Windows toolchain on PATH:** Qt's MinGW compiler and Ninja live in a
  *separate* `<QtRoot>/Tools` install, **not** inside the Qt prefix. The script
  derives `<QtRoot>` from `QT_PATH` and prepends `Tools/mingw*/bin`,
  `Tools/Ninja`, and `Tools/CMake*/bin` to `PATH`, so CMake/Ninja find
  `gcc`/`g++`/`ninja` without you naming the compiler.
- **vcpkg wiring (Windows):** auto-detects a vcpkg install (`$VCPKG_ROOT`,
  `~/vcpkg`, `/c/vcpkg`, …) and passes `-DCMAKE_TOOLCHAIN_FILE` +
  `-DVCPKG_TARGET_TRIPLET=x64-mingw-dynamic` so the render deps
  (libsndfile/libvorbis) are found.

The manual CMake commands below remain valid if you prefer to drive CMake
directly or need a generator the scripts don't use (e.g. MSVC, Xcode).

## Requirements

| Tool          | Version  | Notes                                                                  |
|---------------|----------|------------------------------------------------------------------------|
| CMake         | >= 3.16  | The build system. Verified with 4.3.3.                                 |
| Qt            | Qt6 (Qt5 nominally) | `Core`, `Gui`, `Widgets`, `Xml`, `Network`, `Concurrent`. CMake prefers Qt6 if both are present; only Qt6 is tested. |
| C++ toolchain | C++17    | GCC, Clang, MSVC, or MinGW. Verified with MinGW 13.1.                  |
| libsndfile    | (any)    | Audio file I/O (WAV export). Required on all platforms.                |
| libvorbis + libogg | (any) | Ogg Vorbis audio codec (OGG export). Required on all platforms.   |
| pkg-config    | (any)    | macOS/Linux only: how the render deps are located. Windows uses vcpkg. |

Per-platform audio dependencies are listed below.

## Linux

### 1. Install the prerequisites

Debian / Ubuntu — this is the whole list, and it is the same line `_env.sh`'s
`apt_packages_line()` prints when something is missing (keep the two in step):

```bash
sudo apt install build-essential cmake ninja-build pkg-config git qt6-base-dev \
                 libsndfile1-dev libogg-dev libvorbis-dev libasound2-dev
```

Optional — the libsecret backend of `SSecretStore` (proposal 38):

```bash
sudo apt install libsecret-1-dev
```

It is deliberately NOT in the line above, which is the REQUIRED list and is
mirrored by `_env.sh`'s `apt_packages_line()` ("nothing here is optional").
Without it the credential store falls back to `none`, which means "Remember"
is disabled in the media browser's account dialog; nothing else changes, and
the build succeeds either way.

**Installing it is a CONFIGURE-time change, so re-configure afterwards** —
`./rebuild.sh`, or `cmake` over an existing `build/`. CMake probes
`libsecret-1` with `pkg_check_modules` once, at configure time
(`main/CMakeLists.txt`), so an incremental `./build.sh` after the install will
NOT pick it up and the backend stays `none`. `./rebuild.sh` prints which
backend it settled on; `ctest -R secret_store_test` then names the backend it
actually exercised.

First verified on Ubuntu 2026-08-21 (libsecret 0.21.7), against a real Secret
Service session bus. It had never been compiled before that — and compiling it
took two fixes, because an optional dependency nobody installs is one nobody
builds: the `pkg_check_modules` call was missing `IMPORTED_TARGET` (so *finding*
libsecret broke the CMake generate step), and `ssecretstore_linux.cpp` included
Qt before glib (Qt's `#define signals public` against glib's `signals` member).
The **macOS Keychain backend is still in that state** — written, never built.

| Package | Why it is needed |
|---|---|
| `build-essential` | g++ and the C++17 toolchain. |
| `cmake` (>= 3.16) | The build system. |
| `ninja-build` | The generator the scripts prefer. **Not strictly required** — without it they fall back to CMake's default generator and say so. |
| `pkg-config` | How `tw303a/CMakeLists.txt` finds sndfile/ogg/vorbis on Linux (`pkg_check_modules(... REQUIRED ...)`). Configure fails without it even when the libraries are installed. |
| `git` | Fetches the CLAP/VST3 submodules under `smaragd/third_party/`. Without them the build succeeds but **silently drops plugin hosting**, which disables the `plugin_*` qxa cases. |
| `qt6-base-dev` | Qt 6 `Core` `Gui` `Widgets` `Xml` `Network` `Concurrent`. It also depends on the `qmake6` package, which puts `qmake6`/`qtpaths6` on PATH — that is how the scripts locate Qt with no `QT_PATH` argument (see below). |
| `libsndfile1-dev` | Audio file I/O: WAV export, and general sample import (MP3/FLAC/AIFF/Ogg/Opus). |
| `libogg-dev`, `libvorbis-dev` | Ogg Vorbis export. Both are separate `find_package`/`pkg_check_modules` calls; `libvorbis-dev` does not pull `libogg-dev`'s headers in on every release. |
| `libasound2-dev` | ALSA — `ENABLE_ALSA` defaults **ON** on Linux, and it carries the MIDI half too (the ALSA sequencer, proposal 37 P7a). |

Verified on Ubuntu (development branch) with Qt 6.10.2. Names on other
distributions — **not verified on this repo's boxes**, so treat them as a
starting point:

- **Fedora / RHEL:** `gcc-c++ cmake ninja-build pkgconf-pkg-config git
  qt6-qtbase-devel libsndfile-devel libogg-devel libvorbis-devel
  alsa-lib-devel` (optional `libsecret-devel`)
- **Arch:** `base-devel cmake ninja git qt6-base libsndfile libogg libvorbis
  alsa-lib` (optional `libsecret`)
- **openSUSE:** `gcc-c++ cmake ninja pkg-config git qt6-base-devel
  libsndfile-devel libogg-devel libvorbis-devel alsa-devel`

### 2. Build

```bash
./rebuild.sh          # from the repo root; no QT_PATH needed
```

The distro Qt is found automatically: its prefix is `/usr` and its CMake
package lives under `/usr/lib/<arch>/cmake/Qt6`, which is neither `~/Qt` nor a
Qt-installer "kit" directory — so `_env.sh` asks `qmake6`/`qtpaths6` for the
prefix, and failing that scans for a system `Qt6Config.cmake`. An official Qt
installer kit still wins when it is present and can always be forced
explicitly:

```bash
./rebuild.sh "$HOME/Qt/6.11.1/gcc_64"
```

If a prerequisite is missing, `./rebuild.sh` prints the `apt install` line for
exactly what it could not find before CMake gets a chance to fail.

Driving CMake directly, if you prefer:

```bash
cd smaragd
cmake -B build -G Ninja -DCMAKE_PREFIX_PATH=/usr -DENABLE_ALSA=ON
cmake --build build -j
./build/bin/smaragd
```

### Status

**Linux is under-tested.** The ALSA backend has not been exercised since the
module refactor (xrun recovery was added blind), and every timing figure quoted
in `CLAUDE.md` and `plan/STATE.md` was measured on the Windows box. Expect to
be the first to run the qxa suite here.

Other backends (PipeWire, PulseAudio, JACK) are stubbed in the CMake but their
runtime implementations are not yet wired up. Enable with
`-DENABLE_PIPEWIRE=ON`, etc., once the backends land.

## macOS

```bash
cd smaragd
cmake -B build -G Xcode -DCMAKE_PREFIX_PATH="$(brew --prefix qt@6)"
cmake --build build --config Release
```

Install dependencies via Homebrew:

```bash
brew install qt cmake libsndfile libvorbis
```

Alternatively, for Qt5:

```bash
brew install qt@5 cmake libsndfile libvorbis
```

> **Status:** Builds and runs; the CoreAudio backend is audible with a device
> picker. The build scripts also work here — `./rebuild.sh $HOME/Qt/6.11.1/macos`
> (uses Ninja + clang). The Xcode generator above remains a valid alternative.

### macOS is the strictest compiler this project sees, and it is not a warning level

Apple clang uses **libc++**, and libc++'s headers include far less
transitively than libstdc++'s do. So a translation unit that uses `std::max`
with only `<cmath>` and `<cstdio>` compiles cleanly on the Linux and Windows
boxes this project is developed on and **fails to compile on macOS**:

```
error: no member named 'max' in namespace 'std'; did you mean 'fmax'?
```

`<algorithm>` (`max`/`min`/`sort`/`clamp`/`swap`/`fill`/`copy`), `<cstdint>`
(the fixed-width integer types) and `<cstddef>` (`size_t`) are the three that
bite. There is no CI, and nothing on a Linux or Windows box will ever catch
this — the only defence is **include what you use**, written down here because
a file can pick the habit up by accident and keep it for years: on `main` today
`body_joint_test.cc` uses `std::max` twelve times and got away with it purely
because it also includes `<vector>`.

Adding a new source or test file? Check its `std::` symbols against its
includes before pushing. A one-line grep over the file is cheaper than a
round trip through somebody else's machine.

## Windows

**Verified working:** Qt 6.11.1 + MinGW 13.1 + Ninja, both bundled by the Qt
online installer.

The render deps (libsndfile/libvorbis) are not part of Qt's MinGW kit; they come
from **vcpkg** built for the MinGW-ABI triplet (`x64-mingw-dynamic`).

**Easiest path — use the script** (from Git Bash, repo root). It puts the MinGW
toolchain on PATH, wires up vcpkg, and — if the render deps are missing — runs
`vcpkg install` for you (bootstrapping `vcpkg.exe` first if needed):

```bash
./rebuild.sh /c/Qt/6.11.1/mingw_64
```

The auto-install only triggers when the libs are absent (a one-time cost on a
fresh machine; it can take several minutes as vcpkg builds them with Qt's g++).
It requires a **vcpkg clone** to already exist — the script looks in
`$VCPKG_ROOT`, `~/vcpkg`, `/c/vcpkg`, `/c/Users/*/vcpkg`, and on `PATH`. If you
don't have one yet:

```bash
git clone https://github.com/microsoft/vcpkg "$HOME/vcpkg"
```

To install the deps manually instead (equivalent to what the script runs):

```powershell
# Run with Qt's MinGW bin on PATH so gcc is available to vcpkg.
.\vcpkg install --triplet x64-mingw-dynamic --host-triplet x64-mingw-dynamic libsndfile libvorbis
```

> `--host-triplet x64-mingw-dynamic` is required on a machine without Visual
> Studio: otherwise vcpkg builds its host/build-time tools for the default
> `x64-windows` (MSVC) triplet and fails with "Unable to find a valid Visual
> Studio instance".
>
> Use `x64-mingw-dynamic`, **not** `x64-windows` — the latter is MSVC-ABI and
> won't link against a MinGW build. The Windows build uses the **CMake config
> packages** vcpkg ships (`find_package(SndFile/Ogg/Vorbis CONFIG)`), so it needs
> **no pkg-config / pkgconf** — only macOS/Linux use pkg-config (their system
> libraries ship the `.pc` files for it).

**Manual equivalent** (PowerShell, from `smaragd/`):

```powershell
$env:PATH = "C:\Qt\Tools\mingw1310_64\bin;C:\Qt\Tools\Ninja;" + $env:PATH
cd smaragd
cmake -B build -G Ninja `
  -DCMAKE_PREFIX_PATH="C:/Qt/6.11.1/mingw_64" `
  -DCMAKE_TOOLCHAIN_FILE="C:/Users/<you>/vcpkg/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-mingw-dynamic
cmake --build build
```

The resulting `build\bin\smaragd.exe` is **self-contained and runnable in
place** — `AUTO_DEPLOY_QT` (ON by default) runs `windeployqt` as a post-build
step, copying the Qt runtime, the `platforms\qwindows.dll` plugin, and the
MinGW compiler runtime (`libstdc++-6`, `libgcc_s_seh-1`, `libwinpthread-1`) next
to the exe; vcpkg deploys the audio DLLs alongside. No PATH setup is needed to
launch it. (If you build with `AUTO_DEPLOY_QT=OFF` for faster iteration, you
must instead put Qt's `bin` and the MinGW `bin` on PATH to run.)

Alternative generators (untested but should work once `CMAKE_PREFIX_PATH`
points at a matching Qt build):

```powershell
# MSVC
cmake -B build -G "Visual Studio 17 2022" -DCMAKE_PREFIX_PATH="C:/Qt/6.11.1/msvc2022_64"
cmake --build build --config Release

# Qt5 MSVC
cmake -B build -G "Visual Studio 17 2022" -DCMAKE_PREFIX_PATH="C:/Qt/5.15.2/msvc2019_64"
```

> **Status:** Builds and runs with the Qt 6.11.1 MinGW kit (Ninja). The Phase 2
> audio abstraction has landed, so the POSIX-only includes that previously broke
> the MinGW build are gone. See `plan/STATE.md`.

## Testing Audio Render

After building successfully, test the render feature:

1. **Launch:** `./build/bin/smaragd` (or `..\build\bin\smaragd.exe` on Windows)

2. **Create/open a project** — File → New (or open an existing project)

3. **Test WAV export:**
   - File → Render...
   - Select "WAV" format
   - Entire project / Time selection as available
   - Choose output file (e.g., `/tmp/test.wav`)
   - Click Render; watch progress dialog
   - Verify output file exists and plays in external player

4. **Test OGG Vorbis export:**
   - File → Render...
   - Select "OGG Vorbis" format
   - Adjust quality slider (0-10)
   - Render and verify playback

5. **Test MP3 (if binary provided):**
   - If `libmp3lame.dll/.dylib/.so` is in app directory, File → Render shows MP3 enabled
   - Otherwise, MP3 option is disabled with helpful tooltip
   - If enabled, test bitrate selection and rendering

6. **Stress tests:**
   - Cancel mid-render → verify file cleanup and UI recovery
   - Render long project → verify progress updates smoothly
   - Switch projects during render → should not crash
   - Play synth during render → playback should be blocked (one player at a time)

Expected: Output files are valid, audio is audible, UI remains responsive.

## Testing Audio Recording

The recording feature allows you to capture audio from external input devices (microphone,
line-in, etc.) and place them as clips on armed tracks. To test:

1. **Launch:** `./build/bin/smaragd`

2. **Create/open a project** — File → New (or open an existing project)

3. **Arm a track for recording:**
   - Right-click a track or use the mixer panel
   - Click the ARM button (red "R") to arm the track
   - You can arm multiple tracks; recordings are placed on all armed tracks simultaneously

4. **Select input device:**
   - Edit → Options → Audio tab
   - Choose your input device from the dropdown (e.g., built-in microphone, line-in)
   - Click OK

5. **Start recording:**
   - Press Ctrl+R or use the red Record button in the transport toolbar
   - Speak into the microphone / feed audio into the input device
   - Watch the real-time duration display in the progress dialog

6. **Stop recording:**
   - Click "Stop Recording" in the progress dialog, or press Ctrl+R again
   - The dialog shows completion status
   - A WAV file is written to the project directory with timestamp: `YYYYMMDD_HHMMSS_mmm_input0.wav`
   - The recorded clip appears on all armed tracks at the current playhead position

7. **Stress tests:**
   - Arm/disarm different tracks and re-record; verify clips appear on the right tracks
   - Try different input devices; verify device switching works mid-session
   - Record with playback muted vs. unmuted; verify one operation at a time (mutual exclusion)
   - Record then immediately render; verify both features work in sequence

Expected: Recording completes successfully, clips appear on armed tracks, audio is audible.

### Known Limitations

- **Input enumeration:** Input device list updates on startup only; device plug/unplug during session are not detected
- **Hardware monitoring:** Currently records external input only (no playback + input blend)
- **Sample rate:** Recording uses the project's sample rate; no per-input resampling yet
- **Multi-input:** Single-input only; simultaneous multi-device recording not yet supported
- **Latency control:** No user-facing buffer size control for input device

## Build options

| Option              | Default                | Description                                       |
|---------------------|------------------------|---------------------------------------------------|
| `ENABLE_ALSA`       | ON on Linux            | Build the ALSA audio backend.                     |
| `ENABLE_PIPEWIRE`   | OFF                    | Build the PipeWire backend (Linux, planned).      |
| `ENABLE_PULSEAUDIO` | OFF                    | Build the PulseAudio backend (Linux, planned).    |
| `ENABLE_JACK`       | OFF                    | Build the JACK backend (planned).                 |
| `ENABLE_WASAPI`     | ON on Windows          | Build the WASAPI backend (planned).               |
| `ENABLE_COREAUDIO`  | ON on macOS            | Build the modern CoreAudio backend (planned).     |
