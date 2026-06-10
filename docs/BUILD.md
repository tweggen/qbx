# Building smaragd

The build system is **CMake** (>= 3.16).

All CMake commands below should be run from the `smaragd/` subdirectory of the
repository (the directory that contains `CMakeLists.txt`).

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

> **Status:** The CMake configuration is ready, but `tw303a/src/twspeaker.cc`
> still contains Linux-only POSIX includes (`unistd.h`, `sys/ioctl.h`,
> `syslog.h`) and a deprecated pre-10.5 CoreAudio code path. Compilation will
> fail until the audio abstraction layer (Phase 2) lands.

## Windows

**Verified working (configure step):** Qt 6.11.1 + MinGW 13.1 + Ninja, both
bundled by the Qt online installer.

Install dependencies via **vcpkg** (from your vcpkg clone):

```powershell
.\vcpkg install libsndfile:x64-windows libvorbis:x64-windows
```

Then build:

```powershell
$env:PATH = "C:\Qt\Tools\mingw1310_64\bin;C:\Qt\Tools\Ninja;" + $env:PATH
cd smaragd
cmake -B build -G Ninja `
  -DCMAKE_PREFIX_PATH="C:/Qt/6.11.1/mingw_64" `
  -DCMAKE_TOOLCHAIN_FILE="<path-to-vcpkg>/scripts/buildsystems/vcpkg.cmake"
cmake --build build
```

Alternative generators (untested but should work once `CMAKE_PREFIX_PATH`
points at a matching Qt build):

```powershell
# MSVC
cmake -B build -G "Visual Studio 17 2022" -DCMAKE_PREFIX_PATH="C:/Qt/6.11.1/msvc2022_64"
cmake --build build --config Release

# Qt5 MSVC
cmake -B build -G "Visual Studio 17 2022" -DCMAKE_PREFIX_PATH="C:/Qt/5.15.2/msvc2019_64"
```

> **Status:** CMake configure is green. Compilation fails until Phase 2 lands
> because `tw303a/src/twspeaker.cc`, `twmixer.cc`, `twlatch.cc`, `twpipe.cc`,
> `twcomponent.cc`, `twconstant.cc`, `twsimplesaw.cc` and others
> unconditionally `#include <syslog.h>` / `<sys/ioctl.h>` / `<unistd.h>`,
> which are not present on MinGW. See `plan/STATE.md`.

## Build options

| Option              | Default                | Description                                       |
|---------------------|------------------------|---------------------------------------------------|
| `ENABLE_ALSA`       | ON on Linux            | Build the ALSA audio backend.                     |
| `ENABLE_PIPEWIRE`   | OFF                    | Build the PipeWire backend (Linux, planned).      |
| `ENABLE_PULSEAUDIO` | OFF                    | Build the PulseAudio backend (Linux, planned).    |
| `ENABLE_JACK`       | OFF                    | Build the JACK backend (planned).                 |
| `ENABLE_WASAPI`     | ON on Windows          | Build the WASAPI backend (planned).               |
| `ENABLE_COREAUDIO`  | ON on macOS            | Build the modern CoreAudio backend (planned).     |
