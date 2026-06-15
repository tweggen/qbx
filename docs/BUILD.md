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
`$HOME/Qt/6.11.1/macos`, `$HOME/Qt/6.11.1/gcc_64`). If omitted, it is guessed
per platform; you can also point at any kit explicitly.

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
| Qt            | Qt6 or Qt5 | `Core`, `Widgets`, `Xml` components. CMake prefers Qt6 if both are present. |
| C++ toolchain | C++17    | GCC, Clang, MSVC, or MinGW. Verified with MinGW 13.1.                  |
| libsndfile    | (any)    | Audio file I/O (WAV export). Required on all platforms.                |
| libvorbis     | (any)    | Ogg Vorbis audio codec (OGG export). Required on all platforms.        |

Per-platform audio dependencies are listed below.

## Linux

```bash
cd smaragd
cmake -B build -DENABLE_ALSA=ON
cmake --build build -j
./build/bin/smaragd
```

Required dev packages on Debian/Ubuntu:

```bash
sudo apt install build-essential cmake qtbase5-dev libqt5xml5 libasound2-dev \
                 libsndfile1-dev libvorbis-dev
```

On other distributions, install equivalents:
- **Fedora/RHEL:** `libsndfile-devel vorbis-devel`
- **Arch:** `libsndfile libvorbis`

Other backends (PipeWire, PulseAudio, JACK) are stubbed in the CMake but their
runtime implementations are not yet wired up — see Phase 2 of the
modernization plan. Enable with `-DENABLE_PIPEWIRE=ON`, etc., once the backends
land.

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

## Windows

**Verified working:** Qt 6.11.1 + MinGW 13.1 + Ninja, both bundled by the Qt
online installer.

The render deps (libsndfile/libvorbis) are not part of Qt's MinGW kit, so
install them via **vcpkg** for the MinGW-ABI triplet (built with Qt's g++):

```powershell
.\vcpkg install libsndfile:x64-mingw-dynamic libvorbis:x64-mingw-dynamic pkgconf:x64-mingw-dynamic
```

> Use `x64-mingw-dynamic`, **not** `x64-windows` — the latter is MSVC-ABI and
> won't link against a MinGW build. vcpkg's `pkgconf` also satisfies
> `find_package(PkgConfig)`, which Qt's MinGW kit lacks.

**Easiest path — use the script** (from Git Bash, repo root). It puts the MinGW
toolchain on PATH and wires up vcpkg automatically:

```bash
./rebuild.sh /c/Qt/6.11.1/mingw_64
```

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

To **run** the resulting `build\bin\smaragd.exe`, Qt's `bin` and the MinGW `bin`
must be on PATH (the vcpkg runtime DLLs are auto-deployed next to the exe).

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
