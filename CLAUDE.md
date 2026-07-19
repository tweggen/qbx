# Smaragd Audio Synthesizer

## Quick Summary

**Smaragd** is a Qt6-based audio synthesis application (~11,200 lines of C++) featuring a TB-303 synthesizer clone with granular synthesis. It runs on Windows (WASAPI), Linux (ALSA), and macOS (Null backend pending CoreAudio). Built with CMake, C++17.

The synth engine is in `tw303a/` (static library); the Qt UI and project management live in `main/`.

## Modular layout (2026-07: proposal 14 executed)

**Start here: `docs/ARCHITECTURE.md`** — the module map. Every module has a
`CONTRACT.md` next to its sources (purpose, public headers, invariants,
threading, how to test, known debt). Cross-module protocols are in
`docs/contracts/` (POSITION_DOMAINS, FREEZE_PROTOCOL, THREADING, CLIP_MODEL);
the action-verb reference is `docs/ACTIONS.md`.

- Engine: `tw303a/<module>/` builds one `tw_<module>` static lib each with a
  build-enforced dependency DAG; includes are `tw/<module>/<header>.h`.
- App: `main/<module>/` with `app/<module>/<header>.h` includes, built as ONE
  OBJECT library (`smaragd_app`) — the app is a single strongly-connected
  component until the Phase 6 interface work; OBJECT (not STATIC) is required
  because actions self-register via static initializers.
- Before committing: `python tools/check_layering.py` (module boundaries) and
  the qxa suite from `tests/cases/` must be green.
- Key-file paths below predate the split; the classes are unchanged — find
  headers at `tw303a/<module>/include/tw/<module>/…` and
  `main/<module>/include/app/<module>/…`.

## Architecture: Key Points

### Audio Path
- **Platform abstraction:** All backends implement `AudioBackend` interface (callback-pull model).
- **Sample format/rate are first-class:** Every project has its own sample rate (default 48 kHz, legacy loads as 44.1 kHz). The engine is rate-aware; a resampler at the device boundary reconciles project rate ↔ device rate.
- **Data flows:** Synth graph → `twSpeaker` (holds resampler + format converter) → AudioBackend → device.

### Freeze / rendering model (2026-07: proposal 19 executed — demand-driven dataflow)
- Audio is produced as page-frozen output (`twOutputPage`, 65536 frames). Consumers
  never freeze synchronously: the **offline render** (`RenderSession::setScheduler`)
  and the **playback readahead** (`AudioEngine::setScheduler`) declare *demands*
  (`CaptureRevalidator::requestGraphPages`) and wait/observe at the edge.
- The scheduler expands structural plans (`twComponent::planPage`, per-clip
  resolution via `twView::resolve`) into dependency-counted nodes executed on the
  shared worker pool via `freezePageWithInputs()`; bound input pages are served at
  two seams (`twStreamingLatch::copyData` and the top of `twComponent::freezePage`).
  The same-component predecessor edge gives in-position order + DSP state chaining.
- The **RT audio callback never renders** — enforced by `twRtThreadGuard`
  (one-shot report + assert in `freezePage`); it reads ready pages with the
  stale-predecessor fallback (proposal 16).
- Renders quiesce background aspect jobs via `pauseBackground()` (graph demands
  keep running; a full `pause()` would deadlock them).
- Hard-won invariants and remaining follow-ups (preview lanes, pipelining,
  legacy-pull deletion): `plan/proposed/19_ASYNC_FREEZE_MODEL.md` ("Phase 2
  REVISED") and `plan/proposed/20_DATAFLOW_FOLLOWUPS.md`.

### Testing knobs & determinism gates
- `SMARAGD_REVAL_WORKERS=<n>` overrides the revalidation/scheduler worker count
  (clamped [1,64]); `0` disables the revalidator entirely (legacy pull paths).
- `smaragd/tests/repeat_test.sh <bin> <case.qxa> [N] [workers]` — the flake gate
  (e.g. `takes_group_broadcast` N=50..100, swept over workers {1,4,8,16}).
- Render exactness is gated by **byte-level `cmp` of the rendered WAVs** across
  builds/runs (they are 16-bit PCM — do not parse as float32).

### Supported Platforms
| Platform | Backend | Status |
|----------|---------|--------|
| Windows  | WASAPI  | ✅ Audible, device picker, float32/int16/int32 |
| Linux    | ALSA    | ✅ Implemented (xrun recovery added), untested since refactor |
| macOS    | CoreAudio | ✅ Audible, device picker |
| PipeWire/JACK/PulseAudio | — | ❌ Placeholders only |

## Key Files

**Engine (tw303a/):**
- `include/audio/audio_backend.h` — `AudioBackend` interface, `AudioConfig`, device enum
- `include/twspeaker.h` — audio sink with resampler; connects engine to backend
- `include/twformat.h` — sample format/rate/channels definition
- `include/twconvert.h` — sample format conversion
- `include/twresampler.h` — linear sample-rate converter
- `src/audio/*.cc` — WASAPI, ALSA, Null backend implementations

**App (main/):**
- `include/sapplication.h` — app singleton; owns environment + speaker
- `include/sproject.h` — project state (sample rate, settings)
- `include/ssettings.h` — per-user INI config (selected device, file dialog paths)
- `include/smainwindow.h` — menu system, device picker

**Synthesis:**
- `include/twosc.h`, `twsaw.h`, `twmoog.h`, `twgrainsource.h` — oscillators, Moog filter, grain time-stretch/pitch

## Project Structure

```
plan/
├── STATE.md              # Chronological record of implementation (authoritative)
└── proposed/             # Numbered proposals 02..20; highlights:
    ├── 14_MODULARIZATION.md         (executed — module DAG, CONTRACT.md files)
    ├── 15_SCOPED_INVALIDATION.md    (executed)
    ├── 16_STALE_PAGE_FALLBACK.md    (executed — RT stale-page playback)
    ├── 17_TAKE_LANES_AND_COMPING.md (executed)
    ├── 18_EXACT_POSITION_DOMAINS.md (executed — typed positions, exact maps)
    ├── 19_ASYNC_FREEZE_MODEL.md     (executed — demand-driven dataflow; keep
    │                                 its "Phase 2 REVISED" design current)
    ├── 20_DATAFLOW_FOLLOWUPS.md     (OPEN — preview lanes, pipelining,
    │                                 retirements, housekeeping; start here
    │                                 for the next engine work)
    └── 21_REALTIME_DATAFLOW_INTEGRATION.md (DRAFT — live inputs / live
                                      plugin instruments: live lane +
                                      capture bridge + frontier contract)
docs/
├── PROJECT_OVERVIEW.md   # This document's source
├── ARCHITECTURE.md       # Module map (start here for code navigation)
└── BUILD.md              # Build instructions
```

## Build & Run

**Recommended — the build scripts** (work on macOS, Linux, and Windows/Git Bash;
logic lives in `_env.sh`, sourced by both):

```bash
./rebuild.sh [QT_PATH]   # clean rebuild
./build.sh   [QT_PATH]   # incremental build (auto-configures if build/ is missing)
```

`QT_PATH` is the Qt prefix (e.g. `/c/Qt/6.11.1/mingw_64`, `$HOME/Qt/6.11.1/macos`);
omit it to auto-detect. On Windows the scripts add Qt's bundled MinGW/Ninja to
PATH (the compiler lives in `<QtRoot>/Tools`, *outside* the Qt prefix) and wire
up vcpkg (`-DCMAKE_TOOLCHAIN_FILE` + `x64-mingw-dynamic` triplet) for the render
deps automatically. `AUTO_DEPLOY_QT` defaults ON, so `windeployqt` copies the Qt
runtime + plugins + MinGW runtime next to the exe — the binary is self-contained
and runnable without any PATH setup (`AUTO_DEPLOY_QT=OFF` in the env to skip it).

**Manual (Windows, equivalent):**
```powershell
$env:PATH = "C:\Qt\Tools\mingw1310_64\bin;C:\Qt\Tools\Ninja;" + $env:PATH
cd smaragd
cmake -B build -G Ninja -DCMAKE_PREFIX_PATH="C:/Qt/6.11.1/mingw_64" `
  -DCMAKE_TOOLCHAIN_FILE="<vcpkg>/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-mingw-dynamic
cmake --build build
& .\build\bin\smaragd.exe
```

See `docs/BUILD.md` for platform-specific details.

## Known Issues & Gaps

1. **Linux ALSA:** Untested since refactor (though xrun recovery added).
2. **PipeWire/JACK/PulseAudio:** Placeholders only.
3. **WASAPI:** Shared mode only (no exclusive/bit-perfect).
4. **Resampler:** Linear (pitch-correct, not mastering-grade).
5. **No CI:** Only Windows/Qt6/MinGW regularly tested.
6. **Latency:** Buffer sizing largely fixed; no user-facing control.

## Common Tasks

**Adding a backend:** Implement `AudioBackend` in `tw303a/src/audio/`, wire in CMakeLists.txt and `audio_backend.h::createAudioBackend()`.

**Sample rate changes:** Edit project in `SProject::setSampleRate()`, propagates via `tw303aEnvironment::setSRate()`.

**UI changes:** Add menu items in `SMainWindow`, connect to synth state in `SApplication`.

## Rendering Audio

Smaragd supports exporting audio to file via **File → Render...** menu action. The feature is non-interactive and blocks UI during rendering (maintaining "one player at a time" directive).

### Supported Formats

| Format | Quality Control | Dependency | Notes |
|--------|-----------------|-----------|-------|
| **WAV** | Bit depth (16/24/32) | libsndfile | PCM-only, lossless |
| **OGG Vorbis** | Quality 0-10 | libvorbis | Patent-free, high quality |
| **MP3** | Bitrate 128-320 kbps | libmp3lame (optional) | User-provided binary |

### Render Extent

Users can render either:
- **Entire project:** From start to end of project duration
- **Time selection:** If in/out markers are set (option disabled if unavailable)

### Architecture

**File writers:** `tw303a/src/audio/` implements `AudioFileWriter` interface:
- `WAVWriter` (libsndfile)
- `OGGWriter` (libvorbisenc)
- `MP3Writer` (dynamic dlopen/LoadLibrary for libmp3lame)

**Render session:** `tw303a/src/render_session.cc` manages background thread, pulls synth audio, writes via appropriate writer, emits progress callbacks.

**UI dialogs:**
- `SRenderDialog` (main/src/srenderdialog.cpp) — format/quality/extent selector
- `SRenderProgressDialog` (main/src/srenderprogress.cpp) — modal progress display

**Integration:** `SApplication::startRender()` spawns session; `SMainWindow::onRenderTriggered()` wires menu.

### Optional MP3 Support

MP3 encoding requires `libmp3lame` binary in the application directory due to patent licensing concerns. If not found, the UI disables the MP3 option with a helpful tooltip. Users can obtain the library:

```bash
# macOS: brew install lame → copy /opt/homebrew/lib/libmp3lame.dylib
# Linux: apt install libmp3lame0 → copy /usr/lib/libmp3lame.so
# Windows: vcpkg install lame → copy mp3lame.dll
```

## Recording Audio

Smaragd supports recording from input devices (microphone, line-in, etc.) via **Record** button in the transport toolbar or **Ctrl-R** / **Cmd-R** keyboard shortcut. Recorded audio is automatically converted to clips and placed on armed tracks.

### Recording Flow

1. **Arm tracks:** Click ARM button (red "R") on track headers to select which tracks receive recorded audio
2. **Select input device:** Edit → Options → Audio tab → Input device dropdown
3. **Start recording:** Click record button or press Ctrl-R/Cmd-R
4. **Progress dialog:** Shows real-time duration and allows stopping via "Stop Recording" button
5. **Automatic placement:** On completion, WAV file is converted to `SPlainWave` → `SCut` and placed on all armed tracks at current time position
6. **Auto-disarm:** Armed tracks are automatically disarmed after recording placement

### Architecture

**Audio input abstraction:** `tw303a/include/audio/audio_input.h` defines `AudioInput` interface (platform-agnostic):
- `openDevice(deviceId, sampleRate)` — select input device
- `startCapture()` / `stopCapture()` — control recording stream
- `read(buffer, frameCount)` — pull audio samples (non-blocking)
- `listDevices()` — enumerate available input devices

**Platform implementations:**
- `WASAPIInput` (Windows) — shared-mode capture via WASAPI
- `ALSAInput` (Linux) — ALSA PCM device capture
- `CoreAudioInput` (macOS) — HAL audio unit input (needs read callback implementation)

**Recording session:** `tw303a/src/recording_session.cc` manages background recording thread:
- Creates `AudioInput` for selected device
- Opens WAV output file via `createAudioFileWriter(AudioFormat::WAV)`
- Records loop: pulls frames from input → writes to WAV → emits progress every ~100ms
- Handles stop request gracefully with file cleanup

**UI integration:**
- Transport toolbar: Record button with play/pause icons
- Keyboard shortcuts: Ctrl-R (Windows/Linux), Cmd-R (macOS), numpad * (all platforms)
- Per-track ARM buttons: Red "R" toggle on track control strips (mute/solo area)
- Options dialog: Input device selection (Audio tab)
- Progress dialog: `SRecordingProgressDialog` shows duration MM:SS.mmm, stop button
- Settings: Input device ID persisted in per-machine INI config

**Cut placement:** `SMainWindow::onRecordingCompleted()`:
- Loads recorded WAV as `SPlainWave` object
- Wraps in `SCut` for timeline placement
- Creates `SLink` with timestamp = recording start position
- Parents link to track (UI automatically syncs)
- Places cut once per armed track (one input → multiple track recording)

### Recorded File Format

Files written as WAV (PCM, lossless) in project directory:
- **Filename:** `YYYYMMDD_HHMMSS_mmm_input0.wav` (timestamp with millisecond precision)
- **Sample rate:** Matches project rate
- **Channels:** Stereo (or project channel count)
- **Bit depth:** Float32 (internal engine format)

### Known Limitations & Future Work

1. **CoreAudio input:** Currently placeholder (read returns silence). Full HAL callback integration pending.
2. **Device enumeration:** Only "System default" shown in UI; full platform-specific enumeration deferred to Phase 7b.
3. **Hardware monitoring:** Recording pulls from input device only (no synth-to-recording path). Plugin support on input planned for future phase.
4. **Multi-input:** One WAV per input device; multiple inputs with separate files not yet supported.
5. **Latency control:** Fixed at device default; no user-facing buffer sizing.

## Dependencies

### Core
- **Qt 6** (6.11.x): Widgets, Xml, Core
- **CMake** ≥ 3.16
- **pthreads / std::thread** for backend render threads

### Audio I/O (Required)
- **libsndfile** — WAV export (all platforms)
- **libvorbis / libvorbisenc** — OGG Vorbis export (all platforms)

### Platform-Specific Audio Backends
- **Windows:** WASAPI (SDK: ole32, mmdevapi, avrt, …); MinGW 13.1
- **Linux:** ALSA (libasound)
- **macOS:** CoreAudio

### Optional
- **libmp3lame** — MP3 export (user-provided binary in app directory)
