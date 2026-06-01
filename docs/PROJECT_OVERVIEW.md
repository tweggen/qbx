# Smaragd Audio Synthesizer - Project Overview

## Executive Summary

**Smaragd** is a Qt-based audio synthesis application featuring a software
TB-303 synthesizer clone with granular synthesis capabilities (~11,200 lines of
C++). It builds with **CMake** against **Qt 6** and runs on Windows (WASAPI),
Linux (ALSA), and macOS (CoreAudio).

Audio output goes through a platform-abstracted `AudioBackend` interface, and
sample format/rate are first-class properties of the signal path: every project
has its own sample rate, the engine is rate-aware, and a resampler at the device
boundary reconciles the project rate with whatever the audio device runs at.

> **History / current status:** this document is a high-level snapshot. The
> authoritative, chronological record of what has been implemented (and what is
> deferred) lives in [`plan/STATE.md`](../plan/STATE.md); design proposals live
> in [`plan/proposed/`](../plan/proposed/).

## Project Structure

```
smaragd/
├── CMakeLists.txt              # Top-level project: C++17, Qt6, platform + backend options
├── tw303a/                     # TB-303 synthesizer engine (static library)
│   ├── CMakeLists.txt
│   ├── include/
│   │   ├── twcomponent.h       # Base component + the plug/latch "wire" model
│   │   ├── twformat.h          # twFormat (rate/type/channels) + capability types
│   │   ├── twconvert.h         # Shared sample-format converter
│   │   ├── twresampler.h       # Linear sample-rate converter
│   │   ├── twnegotiator.h      # Graph-wide format negotiation
│   │   ├── twosc.h / twsaw.h / twmoog.h / twgrainer.h  # Oscillators, filter, granular
│   │   ├── twspeaker.h         # Audio sink: resampler + AudioBackend (KEY FILE)
│   │   └── audio/
│   │       ├── audio_backend.h # AudioBackend interface, AudioConfig, AudioDeviceInfo
│   │       ├── wasapi_backend.h
│   │       ├── alsa_backend.h
│   │       └── null_backend.h
│   └── src/                    # Implementations (incl. src/audio/*.cc)
├── main/                       # Qt application (UI, project management)
│   ├── CMakeLists.txt
│   ├── include/
│   │   ├── sapplication.h      # App singleton; owns the environment + speaker
│   │   ├── sproject.h          # Project/session state (incl. sample rate)
│   │   ├── ssettings.h         # Per-user config store (QSettings INI)
│   │   ├── smainwindow.h       # Main UI window (menus, device picker)
│   │   ├── sstdmixer.h / strack.h / sstdmixerview.h
│   │   └── ...
│   └── src/
├── include/                    # Shared utilities (exceptions, logging shim)
├── doc/ , images/ , pix/       # Notes, screenshots, XPM icons
└── docs/                       # This overview, BUILD.md
```

## Audio Architecture

### Abstraction layer

Platform `#ifdef` sprawl was replaced by a single interface
(`tw303a/include/audio/audio_backend.h`):

- **Callback-pull model:** the backend owns the timing and calls a
  `RenderCallback` to pull interleaved float frames from the synth.
- **`AudioConfig`** reports the rate, channel count, buffer/period sizes, and the
  device's native binary `sampleType` (float32 / int16 / int32).
- **Rate negotiation:** `supportedRates()` advertises the rates the device can
  open without host resampling; `openDevice(device, preferredRate)` requests a
  rate; `getConfig()` reports what was actually opened.
- **Device enumeration:** `enumerateDevices()` returns selectable
  `{id, name}` endpoints for the device-picker UI.
- `createAudioBackend()` selects the backend compiled in for the platform; an
  always-available `NullBackend` lets the app run silently when no real backend
  is enabled.

### Supported platforms

| Platform | Backend | Status | Notes |
|----------|---------|--------|-------|
| Windows  | WASAPI  | ✅ Implemented, **verified audible** | Shared mode, event-driven render thread, MMCSS "Pro Audio"; device enumeration + selection; float32/int16/int32. |
| Linux    | ALSA    | ✅ Behind the abstraction | Extracted from the old code, **plus xrun recovery**; S16_LE. Not re-tested on Linux since the refactor. |
| macOS    | CoreAudio | ✅ Implemented, **audible** | Modern CoreAudio backend; device enumeration + selection. Build with `-DENABLE_COREAUDIO=ON`. |
| Linux    | PipeWire / JACK / PulseAudio | ❌ Not implemented | CMake wiring + `pkg_check_modules` present; backends are placeholders. |
| (legacy) | OSS, old CoreAudio | 🗑️ Removed | The `/dev/dsp` socket-notifier path and the OS X 10.2 `OpenAComponent` path were deleted. |

### Sample format & rate handling

Data format is a property of every "wire" (the `twLatch` → `twLatchStreamingOutput`
connection), not an engine-wide constant:

1. **`twFormat`** (rate, binary sample type, channels, layout) is attached to
   each producing latch and queried by its consumer. The default is mono float32,
   byte-identical to the engine's historic assumption.
2. **Per-project sample rate.** `SProject` stores a sample rate (a fresh project
   defaults to **48 kHz**; legacy files without the attribute load as 44.1 kHz)
   and a configurable candidate-rate set, both persisted in the project XML and
   pushed into the engine (`tw303aEnvironment::setSRate`).
3. **Rate-aware engine.** Oscillators, the delay line, the Moog filter, tempo
   math and the WAV writer all derive their constants from `env.getSRate()`
   rather than a hardcoded 44.1 kHz.
4. **Resampling at the device boundary.** `twSpeaker` holds a `twResampler` that
   converts the graph rate to the rate the device actually opened at — a
   passthrough when they match. This removed the long-standing ~8.8 % pitch error
   on 48 kHz devices.
5. **Format conversion.** A shared `twConvertFrames` handles type/channel
   conversion (e.g. float → int16) at the device boundary and in the WAV writer.
6. **Negotiation.** `twNegotiator` resolves a single rate per wire across the
   graph (an arc-consistency fixpoint over a finite candidate-rate domain),
   folding in the device's advertised rates, and runs before playback. It is
   currently advisory — the speaker's resampler guarantees correct output
   regardless — and live insertion of in-graph resampler nodes is deferred.

### Configuration & UI

- **`SSettings`** (`main/`) is a `QSettings`-based per-user INI
  (`%APPDATA%/Smaragd/smaragd.ini`, `~/.config/Smaragd/smaragd.ini`) holding
  machine-local settings that don't belong in a project file: the selected audio
  device and the last-used directories for file dialogs.
- **Device picker:** an **Audio → Output Device** menu lists enumerated
  endpoints; the choice is persisted and restored at startup.

## Known Issues & Limitations

1. **PipeWire / JACK / PulseAudio** — not implemented (placeholders only).
2. **ALSA untested since the refactor** — behaviour-preserving rewrite + xrun
   recovery, but needs a Linux smoke build.
3. **WASAPI shared mode only** — no exclusive (bit-perfect) mode; a device whose
   mix rate differs from the project is bridged by the resampler rather than
   opened natively.
4. **Resampler is linear** — adequate to fix pitch/speed; not mastering-grade.
5. **No CI** — no GitHub Actions workflow yet; only the Windows/Qt6/MinGW build
   is regularly exercised.
6. **Latency** — buffer sizing is largely fixed; no user-facing latency control.

## Codebase Statistics

- **Total**: ~11,200 lines (C++/C), up from ~7,200 pre-modernization.
- **Synthesizer (`tw303a/`)**: oscillators, Moog filter, granular synthesis,
  effects, the wire/format/resampler/negotiator machinery, and audio backends.
- **Main app (`main/`)**: Qt UI, project management/serialization, settings.

## Key Dependencies

- **Qt 6** (6.11.x): Widgets, Xml, Core.
- **CMake** (≥ 3.16) — the sole build system.
- **Windows**: WASAPI (built-in SDK: `ole32`, `mmdevapi`, `avrt`, …); MinGW 13.1.
- **Linux**: ALSA (`libasound`); optional PipeWire/JACK/PulseAudio via pkg-config.
- **pthreads / std::thread**: backend render threads.

## Build

See [`docs/BUILD.md`](BUILD.md). On Windows the typical flow is:

```powershell
$env:PATH = "C:\Qt\Tools\mingw1310_64\bin;C:\Qt\Tools\Ninja;" + $env:PATH
cd smaragd
cmake -B build -G Ninja -DCMAKE_PREFIX_PATH="C:/Qt/6.11.1/mingw_64"
cmake --build build
& .\build\bin\smaragd.exe
```

## Next Steps

See `plan/proposed/` for detailed strategies and `plan/STATE.md` for current
status. Open threads include: a modern CoreAudio backend, a Linux ALSA smoke
build, CI, exclusive-mode / per-device rate selection, and the SAction
command/undo/scripting model (`03_ACTION_MODEL.md`, design only).
