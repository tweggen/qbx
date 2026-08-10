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
- Before committing: `python tools/check_layering.py` (module boundaries),
  `python tools/check_logging.py` (no direct stderr/stdout writes — everything
  goes through `TW_LOG*` / `syslog()`, proposal 24) and the qxa suite from
  `tests/cases/` must be green.
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

### External file references in a .qxp (2026-07-28)

A sample path is stored PORTABLY, by three rules
(`main/model/include/app/model/sfilepathref.h`; `SFilePathRef::toStored` /
`fromStored` are the only encoders):

1. **relative to the project file** — the default;
2. **relative to `~`** (`~/audio/lib/kick.wav`) when the relative path would
   have to climb all the way up TO the home directory;
3. **absolute** only when the climb goes past home to a root, or the paths
   share no root at all (different Windows volumes).

The anchor is `SProject::projectFilePath()`, set by `SSaveProjectAction` and
`SLoadProjectAction` — never the serialized `fileName` attribute. In memory
`SPlainWave::fileName_` stays ABSOLUTE; only the serializer and the loader see
a stored spelling. A relative reference that does not resolve next to the
project file falls back to its raw form, so the .qxa runner's sample base dir
(`setSampleBaseDir`) and older projects still load exactly as before. Plugin
module paths (`<SPluginSlot path=…>`) are NOT in scope — they resolve against
the plugin search paths. Gates: `filepathref_test` (ctest) and
`sample_path_portable.qxa`.

### Testing knobs & determinism gates
- `SMARAGD_REVAL_WORKERS=<n>` overrides the revalidation/scheduler worker count
  (clamped [1,64]); `0` disables the revalidator entirely (legacy pull paths).
- `TW_STRETCH_BACKEND=vocoder|ola` picks the time-stretch backend for the run
  (default `vocoder`; `ola` is the legacy overlap-add reference). Read once
  per process, so the choice is deterministic within a run.
- `SMARAGD_SIDECAR_DIR=<path>` relocates the derived-data (QAF) sidecar cache;
  `SMARAGD_SIDECAR_DIR=off` disables it — the store then misses/no-ops and the
  engine result is unchanged, only slower (sidecars alter latency, never output).
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
└── proposed/             # Numbered proposals 02..34; highlights:
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
    ├── 21_REALTIME_DATAFLOW_INTEGRATION.md (DRAFT — live inputs / live
    │                                 plugin instruments: live lane +
    │                                 capture bridge + frontier contract)
    └── 34_LEVEL_METERS.md           (executed 2026-08-05 — level meters read
                                      frozen pages BY POSITION; zero engine
                                      edits. Read it before touching metering:
                                      the naive freeze-time design is wrong)
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

## Development workflow (worktrees, branches, PRs)

**A PR is the only route to `main`.** Every landing since #5 is a merge commit from a
branch; nothing is pushed to `main` directly. The author merges — an agent opens the PR
and stops there.

**There is no CI** (`.github/` does not exist), so the gates below are the *only* thing
that will ever check a branch. Running them is not optional diligence; it is the entire
safety net.

### One worktree per branch

```bash
git worktree add .claude/worktrees/<slug> -b fix/<slug> main
cd .claude/worktrees/<slug> && ./build.sh
```

`.claude/worktrees/` is gitignored ("never track as gitlinks"). The main checkout stays on
`main`, buildable and undisturbed, so a second effort can start without invalidating the
first one's incremental build.

Two things that cost time if rediscovered the hard way:

- A fresh worktree has **no `smaragd/build/`** — the first `./build.sh` is a full
  configure + compile, not an incremental one. Budget for it, or work in the main checkout
  when the branch is already based on `main` and the delta is small. A docs-only branch
  needs no build at all.
- The clap/vst3 submodules are fetched **per worktree**. `ensure_submodules()`
  (`_env.sh:150`) handles it: it tests both `-d .git` and `-f .git`, and in a worktree
  `.git` is a *file*. Without the submodules the build still succeeds — it just silently
  drops CLAP/VST3 hosting, which quietly disables the `plugin_*` qxa cases.

After the PR merges: `git worktree remove .claude/worktrees/<slug>` and delete the branch.

### Branch naming

Keep the existing prefixes — `feat/`, `fix/`, `docs/` — and carry the issue key when there
is one: `fix/QBX-123-nested-lane-solo`.

### YouTrack

Issues live at **https://nassau.youtrack.cloud**, project key **QBX**. Where an issue
exists, it is the anchor: its key goes in the branch name, its URL in the PR body, and it
gets a comment or a state change when the PR lands.

JetBrains ships an official remote MCP server (YouTrack 2025.3+) at
`https://nassau.youtrack.cloud/mcp`, so an agent can read and update issues directly.
Connect it **per user, never per project**:

```bash
claude mcp add --scope user --transport http \
  --header "Authorization: Bearer <permanent-token>" \
  youtrack https://nassau.youtrack.cloud/mcp
```

Token: YouTrack → Profile → Account Security → Authentication → New token, scope
*YouTrack*. Requests run with that user's own permissions. **Do not use `--scope project`**
— that writes `.mcp.json` into the repo and would commit the token.

### Gates, before every PR

```bash
./build.sh                                   # re-configures: required, see below
python3 tools/check_layering.py              # module boundaries
python3 tools/check_logging.py               # no direct stderr/stdout writes
ctest --test-dir smaragd/build --output-on-failure
```

- **The re-configure is load-bearing.** The qxa glob in `smaragd/CMakeLists.txt` is
  `CONFIGURE_DEPENDS`; without a configure pass a newly added `.qxa` is never registered
  and `ctest` reports all-green while never having run it.
- **Reconcile the count**: registered vs run vs skipped. A silently-unregistered case is a
  failure mode this repo has actually hit.
- **Run DSP-sensitive cases first and separately** (`grain_*`, `exact_*`, `stress_*`,
  `warp_*`) when the change touches page freezing, invalidation or predecessor chaining —
  they are the ones most able to be perturbed.
- **A case that fails once and passes on re-run is not a pass.** Pin it with
  `smaragd/tests/repeat_test.sh <bin> <case.qxa> [N] [workers]`, swept over
  `SMARAGD_REVAL_WORKERS` {1,4,8,16}, before deciding it is a flake. Report it either way.

### What a PR body must say

What was gated, **and what was not**. Concurrency and latency properties of the live
playback path routinely have no bespoke gate — a timing assertion tight enough to separate
the behaviours would be flaky. Say so explicitly rather than letting a green suite imply
coverage that does not exist. Unreproduced flakes get named too.

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

## Level meters (proposal 34 — executed 2026-08-05)

Per-track meters in the arranger track heads, one in the Track Detail dock, and a
master meter in the transport toolbar: peak bar + held peak tick + a 300 ms RMS
bar inside it + a latching clip cap, on a −60…+6 dBFS dB-linear scale, all
latency-compensated. Design: `plan/proposed/34_LEVEL_METERS.md`. Invariants:
`tw303a/metering/CONTRACT.md` and `main/timeline/CONTRACT.md` inv. 10-12.

**Read this before touching metering — the obvious design is wrong.** Levels are
read from FROZEN PAGES **by position** (`twLevelProbe` → `getPageIfExists`), never
computed at freeze time. Pages are frozen by readahead/revalidator workers far
ahead of the playhead (65536 frames ≈ 1.37 s at 48 kHz) and by renders with no
playhead at all, so a freeze-time peak stashed in a per-track atomic would show
the FUTURE. Reading by position is inherently "what is audible", which is why the
whole feature needed **zero edits to any existing engine file** — just the new
`tw/metering` leaf plus app code.

| Thing to know | Why |
|---|---|
| The tap is a track's ROOT component (`STrack::getRootComponent()` → its `twRewire`) | It is the only per-track component that CACHES pages: `twTrackMix::freezePage` allocates a fresh page every call, `twPluginChain::freezePage` forwards to its last insert. Content there is post-fader, post-FX, pre-summing. Consequence: a pre-fader meter is not available without new engine work. |
| `outputLatencyFrames` is in DEVICE frames at the DEVICE rate | The locator counts PROJECT frames. `SApplication::meterLatencyFrames()` scales by `projectRate/deviceRate`; skipping that is a ~9% error for 44.1 k on a 48 k device. Applied ONCE in the pump so all meters share one position. |
| Ballistics live on the UI thread, driven by wall-clock dt | Frame-rate independence (one 1 s step == 100 × 10 ms steps) is asserted by `metering_test` and is the reason they are not in the engine. |
| `meterTimer_` is NOT a fold into `pumpLocator` | `pumpLocator` only works when the position changed and stops the instant playback stops. Meters need a tick at a static position (to decay) plus a ~8 s tail, or the bars freeze mid-level. Not started during an offline render; started while recording. |
| A page miss must DECAY the meter | `advanceTo()` returning false → `idle()`. A dropout then reads as a fast fall, never as a frozen bar. Nothing here may block, wait, or create a demand. |
| Stale-but-frozen pages are deliberately ACCEPTED | Playback serves exactly those (proposal 16), so rejecting them would make the meter disagree with the ear while an edit is absorbed. |
| Mono | `SStdMixer` runs one bus and `freezePage_nolock` renders `idx = 0`. Never assert `L != R`. |
| `twAspectMetadata` stays unclaimed | `freezePage` already stores `validAspects = twAspectAll`, so that "peak levels" bit is already set and already meaningless. Claiming it would drag metering into the demand system for nothing. |

**One engine hole this exposed** (not a product bug, but it shapes tests): the
LEGACY PULL path does not observe a track-gain change made after a position was
first frozen — `twStreamingLatch::copyData` gates its cached page on the
**`twPluginChain`'s** content epoch, which `STrack::invalidateRenderPath()` does
not reach (the same "an `SPluginChain` is not an `SLink` child of its track"
pitfall `plugins/CONTRACT.md` records for slots). Playback and render both go
through the scheduler, which re-plans and re-binds, so both see it. `assert-meter`
drives the legacy pull, so set a gain BEFORE first probing a position —
`meter_postfader.qxa` uses two tracks at different gains rather than changing one
track's gain twice.

There is now ONE volume-fader curve, `app/timeline/sfadercurve.h`. The Track
Detail dock's slider used to be wired to nothing and to map `value = dB*10`,
disagreeing with the arranger's `VOLUME_CURVE_EXPONENT = 0.5`; both now share the
curve and commit through `SSetTrackVolumeAction`.

Gates: `ctest -R metering_test` and the qxa cases `meter_levels` (per-second RMS
of the ramped-sawtooth fixture, the miss/silence path, the density rules via the
REAL head built off screen, plus PNG grabs — the only coverage of
`SLevelMeter::paintEvent`) and `meter_postfader`.

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

## Plugin Hosting (proposal 08 — M0..M8 executed; VST3 landed 2026-07-29)

CLAP, **VST3** and **AudioUnit** (macOS) audio-effect plugins are scanned,
inserted per track, heard in the signal path, saved with the project, and kept as
a reloadable placeholder when the plugin is not installed. **Design:**
`plan/proposed/08_PLUGIN_HOSTING.md`; **what was built and in what order:**
`plan/todo/08_PLUGIN_HOSTING_EXECUTION.md`; **the invariants that matter:**
`smaragd/tw303a/plugins/CONTRACT.md` (26 of them) and
`smaragd/main/pluginui/CONTRACT.md`. The milestone list is closed — remaining
work is coverage, not capability.

On macOS a `.clap` may be a directory bundle or a flat dylib — `twClapModule`
handles both (`stat` the path; bundles resolve the inner binary from
`Contents/MacOS`, preferring the base name), and the app needs the
`com.apple.security.cs.disable-library-validation` entitlement to dlopen
unsigned third-party plug-ins under the ad-hoc signature. A `.vst3` has the same
split on **every** platform — a flat DLL renamed `.vst3` or a
`Contents/<arch>-win|linux|MacOS/` bundle — and `twVst3Module` resolves both.

**VST3 (M6) proved proposal 08 AC 4:** a second format really was only a new
backend. Four new files under `tw303a/plugins/src/` (`twvst3module`,
`twvst3plugin`, `twvst3host`, `twvst3iids`) and **no change** to the
processor/tap split, the model, the actions or the UI. Three things to know:
`IEditController::setParamNormalized` never reaches the DSP — a parameter edit
must travel as `ProcessData::inputParameterChanges`; `vst3_pluginterfaces` ships
no `vstinitiids.cpp`, so `twvst3iids.cc` defines the VST module IIDs itself (a
build without it links clean and dies on the first `IComponent::iid`); and the
submodule directory is named `vst3_pluginterfaces` while the SDK headers include
each other as `pluginterfaces/...`, so CMake mirrors them into
`${CMAKE_CURRENT_BINARY_DIR}/vst3_inc/pluginterfaces` at configure time.

**AudioUnit (macOS, M8):** a second format behind the same `twPlugin` interface,
so the model / serialization / processor-tap / UI are unchanged (proposal 08 AC
4). Unlike CLAP, AU is discovered from the **OS component registry**
(`AudioComponentFindNext`), not by walking directories: a "module" is one
component, keyed `au:<type>-<subtype>-<manufacturer>` (hex), and a descriptor's
`uid` is that triple with an EMPTY `path` (AU instantiates from the component
description, so AU projects re-resolve by uid and are portable without a path).
Hosting is the plain C AudioUnit API (`twaumodule.cc` / `twauplugin.cc`, no
Obj-C); state is `kAudioUnitProperty_ClassInfo` in a `'TWAU'` frame. Backend
files are PRIVATE to `tw_plugins` behind `TW_HAVE_AU`. `SMARAGD_SCAN_AU=0`
suppresses AU enumeration (the headless scan gate uses it; insert/instantiate go
by descriptor and never scan). Test gating is stock-system-AU based (no in-repo
`.component` fixture): `au_test` + the macOS-only `au_*.qxa` cases, which use a
qualitative RMS discriminator (AULowpass), never a byte-`cmp`.

### Layers

| Piece | Where | What it is |
|---|---|---|
| ABI | `tw/plugins/twplugin.h` | `twPlugin`: `prepare`/`process`/`reset`, params, opaque state blob. Format-agnostic and deliberately narrow. |
| CLAP backend | `plugins/src/twclapmodule.{h,cc}`, `twclapplugin.cc` | DSO load (`LoadLibraryExW` / `dlopen`, macOS bundles → `Contents/MacOS/<base>`), `clap_entry`, modules interned by path. Maps audio-ports / params / state / latency / gui. |
| VST3 backend | `plugins/src/twvst3module.{h,cc}`, `twvst3plugin.cc`, `twvst3host.{h,cc}`, `twvst3iids.cc` | DSO/bundle load (`InitDll` / `bundleEntry` / `ModuleEntry` → `GetPluginFactory`), modules interned by path. `IComponent` + `IAudioProcessor` + `IEditController`, both the single-component and split-controller shapes. Params NORMALIZED [0,1]; edits reach the DSP only via `inputParameterChanges`. |
| AU backend (macOS) | `plugins/src/twaumodule.{h,cc}`, `twauplugin.cc` | Plain C AudioUnit API, no Obj-C. NOT directory-scanned — enumerated from the OS component registry (`AudioComponentFindNext`); a "module" is one component keyed `au:<type>-<subtype>-<manufacturer>` and a descriptor's `path` is EMPTY. |
| Scanner + cache | `plugins/src/twpluginregistry.cc`, `twpluginsearchpaths.cc`, `twpluginscancache.cc` | Search paths, `plugincache.json`, mtime/size/version keying, sticky `failed`/`timeout` records, background scan. |
| Crash isolation | `plugins/tools/plugin_probe.cc` → `smaragd_pluginprobe` | One module per child process, driven by `QProcess` with a timeout. A crash becomes a cache record, not a dead app. |
| Slot DSP | `plugins/include/tw/plugins/twpluginslotproc.h` + `src/twplugininsert.cc` | **One processor + N per-bus taps.** See below. |
| Placeholder | `plugins/src/twnullplugin.cc` (`createNullPlugin`) | Inert pass-through with the *declared* I/O of a missing plugin, so the graph shape is already the one the real plugin will get. |
| Model | `main/objects/track/spluginchain.cpp`, `spluginslot.cpp` | `SPluginChain` (ordered container) + `SPluginSlot` (descriptor, state chunk, bypass, `reloadPlugin()`). |
| Actions | `main/objects/track/s{insert,remove,reorder}plugin*.cpp`, `ssetplugin{bypass,param}action.cpp` | `insert-plugin`, `remove-plugin`, `reorder-plugin`, `set-plugin-bypass`, `set-plugin-param` — see `docs/ACTIONS.md`. |
| UI | `main/pluginui/` | Browser dialog, FX strip (mounted from `main/timeline/src/strackdetailpanel.cpp`), generic parameter editor. |
| Options | `main/servicesui/src/soptionsdialog.cpp` | The Plugins page: directory list, Rescan now, scan status. |

### The processor / tap split (why a slot is not one component)

The frozen-page model is **one mono page per component**
(`twComponent::freezePage_nolock` renders `idx = 0` only), so N parallel mono
wires are N parallel component *instances* — which is why `STrack` builds one
`twTrackMix` and one `twPluginChain` **per bus**. But a stereo-linked plugin has
to see all its channels in one `process()` call, which no single mono component
can express. Hence: one `twPluginSlotProcessor` per slot (plain C++, not a
`twComponent`) owning the plugin instance(s), the bypass flag, the block
chunking (4096 frames out of a 65536-frame page), the channel-mismatch mapping
and a small all-bus page cache; plus one `twPluginInsert` **tap** per bus, each
strictly 1-in/1-out. The first tap to ask renders every bus; the rest hit the
cache. Channel-mismatch mapping is derived once from the plugin's *own* reported
layout: `N→N` Direct, `1→1` on N buses DualMono (N instances — hence a *factory*,
not one instance), `2→2` on one bus MonoFold, anything else `Unsupported` and
transparent.

**Two caches sit in front of a plugin edit**, and a third thing above it: the
processor's all-bus cache, each tap's frozen pages, and the components
downstream. A parameter write must be followed by
`SPluginSlot::notifyPluginEdited()`, and a bypass must go through
`SPluginSlot::setBypass()`; both emit `SPluginSlot::audioInvalidated()`, which
the owning `STrack` turns into `invalidateRenderPath()`. The slot cannot do that
last step itself — an `SPluginChain` is deliberately *not* an `SLink` child of
its track, so `SObject::invalidateRenderPath()`'s root-down walk never reaches a
slot. Skip any of the three and the edit is completely inaudible.

### Serialization

```xml
<SPluginSlot id='…' bypassed='false' format='clap' uid='…' name='…' vendor='…'
             path='…' nIn='2' nOut='2' isInstrument='false'>
  <state encoding='base64'>…</state>
</SPluginSlot>
```

`descriptor_` is written verbatim — never the registry-resolved one — so a
relative module path stays relative and a project stays portable, and a plugin
missing on *this* machine keeps its identity across a save. The state chunk is
pulled **fresh from the live plugin** at save time, and is *never* written for a
non-Active slot (the placeholder's chunk is empty; writing it would destroy the
user's patch). `STrack` references its chain with `pluginChainId='…'` and adopts
it in `SProjectLoader::deferResolve`, because a plain attribute is invisible to
the loader's `<SLink>`-based ordering. CLAP state blobs are wrapped in our own
8-byte frame (`'TWCP'`, u16 version, u16 reserved); a newer version is refused,
not guessed at. VST3 blobs use `'TWV3'` plus **two length-prefixed chunks**
(component, controller) — a distinct magic so a mis-routed blob is refused rather
than misread, and the controller chunk is written only for a *separate*
controller (a single-component plugin implements both `getState`s with one
virtual, so storing it twice is pure duplication).

### Knobs

| Knob | Effect |
|---|---|
| `TW_HAVE_CLAP` (CMake, **PRIVATE** to `tw_plugins`) | Set when `smaragd/third_party/clap/include/clap/clap.h` exists (a git submodule, fetched by `ensure_submodules()` in `_env.sh`). Without it the build compiles, warns, and skips the CLAP half of `plugins_test`. It must never enter a public `tw/plugins/*.h` — that would be ODR/ABI skew. |
| `<configDir>/plugincache.json` | The scan cache (next to `smaragd.ini`). One record per module: path, size, mtime, scanner version, `ok`/`failed`/`timeout`, and the descriptors found. Delete it, or use Rescan with *force*, to clear sticky failures. |
| `smaragd_pluginprobe[.exe]` | The out-of-process probe; the **app** supplies its path (next to the exe; inside `Contents/MacOS` on macOS). Absent ⇒ the scan falls back in-process and logs a warning — safe against a corrupt file, not against a plugin that crashes on instantiation. |
| `TW_HAVE_VST3` (CMake, **PRIVATE** to `tw_plugins`) | Set when `smaragd/third_party/vst3_pluginterfaces/base/funknown.h` exists. Same PRIVATE discipline and the same consequences as `TW_HAVE_CLAP`. It also gates `formatForFile()` reporting `.vst3`, so a build without the submodule cannot cache an unloadable module as a permanent failure. |
| `plugins/searchPaths`, `plugins/scanOnStartup` (`SOpt`) | Edited on Edit → Options → Plugins. Defaults are the union of `twPluginSearchPaths::defaults("clap")` and `…("vst3")`, de-duplicated: Windows `%CommonProgramFiles%\{CLAP,VST3}` + `%LOCALAPPDATA%\Programs\Common\{CLAP,VST3}` + `CLAP_PATH`/`VST3_PATH`; macOS `/Library/Audio/Plug-Ins/{CLAP,VST3}` + `~/Library/…`. `SOpt::def()` and `SOptionsDialog::resetPluginDirsToDefaults()` must stay in step. |
| `TW_HAVE_AU` (CMake, **PRIVATE** to `tw_plugins`) | Set on macOS unless `-DENABLE_AU=OFF`. Nothing to fetch — the AudioToolbox/AudioUnit headers ship in the macOS SDK. Same PRIVATE discipline as the other two. |
| `SMARAGD_SCAN_AU=0` | Suppresses AU enumeration from the OS component registry. The headless scan gate needs it (it asserts exact module counts against a controlled fixture dir); insert/instantiate go by descriptor and never scan, so it never affects the qxa cases. |
| `clap_probe`, `vst3_probe` (targets, not gates) | `plugins/tools/{clap_probe,vst3_probe}.cc` — load a real third-party plugin with the production loader and print what it offers. `vst3_probe` was the M6 ABI spike and walks a whole lifecycle (instantiate → buses → params → state → `process()` → teardown); it is the fastest way to triage "this one plugin will not load" without starting the app. |

`formatForFile()` reports `*.clap` and `*.vst3` and nothing else — `.component`
is deliberately absent, because AU is enumerated from the OS component registry
rather than by walking directories (`SMARAGD_SCAN_AU=0` suppresses that
enumeration for the count-exact headless scan gate). That list is
deliberately conservative: a module format we cannot load would be probed, fail,
and be cached as a *permanent* failure a later milestone would have to
force-clear — which is exactly why `.vst3` stayed unreported until M6.

### Testing without installing anything

`plugins/tests/twtestclap.c` is a real 2-in/2-out CLAP module built from this
repo as `twtestclap.clap` and copied next to the binary. Two entry points:
`tw.test.clap.gain` (`out = in * gain`, plus a "report block size" mode that
writes the frame count it actually saw) and `tw.test.clap.stereoskew`
(`out[0] = in[0]*0.5*gain + in[1]*gain`, `out[c>=1] = in[c]*0.5*gain`) — the
cross-channel term is what makes a silent second input visible in a mono render.

`plugins/tests/twtestvst3.cpp` is the VST3 counterpart — a real 2-in/2-out VST3
built as `twtestvst3.vst3`, one `Gain` parameter, unity by default. It is C++
because VST3's ABI *is* a C++ vtable, and it links its own copies of the SDK
sources (a module and its host are separate binaries). It **deliberately ignores
`setParamNormalized`**, so a host that writes the controller and stops there
fails the level assertion — the most common VST3 host bug, made into a
regression test. It is a *single component*; the split component/controller
shape has no automated coverage (recorded in `plugins/CONTRACT.md` known debt).

Gates: `ctest -R "plugins_test|plugins_scan_test"` and the qxa cases
`plugin_stereo_chain`, `plugin_remove_and_undo`, `plugin_slot_roundtrip`,
`plugin_missing_placeholder`, `plugin_bypass_and_param`,
`plugin_remove_restores_param`, `plugin_ui_strip_and_editor`,
`render_sawtooth_with_effects`.

**Known gap that is not the plugin layer's:** the audio sink is still mono.
`RenderSession` and `AudioEngine` collapse the graph's buses to one page and
duplicate it, so bus 1 cannot reach a file or a device — a rendered WAV's two
channels are equal *by construction*. Never write a qxa assertion of the form
`L != R`; use a cross-channel fixture or an RMS discriminator.

## Dependencies

### Core
- **Qt 6** (6.11.x): Widgets, Xml, Core
- **CMake** ≥ 3.16
- **pthreads / std::thread** for backend render threads

### Audio I/O (Required)
- **libsndfile** — WAV export (all platforms) AND general sample **import**:
  every inserted sample except 16-bit-PCM WAV (which keeps a byte-exact
  hand-rolled fast path) decodes through libsndfile — MP3, FLAC, AIFF, Ogg,
  Opus, and non-16-bit WAV. `tw_sources` links it (mirroring `tw_sinks`);
  `twSampleSource::loadSndfile()` yields the same planar-Float32 buffer as the
  WAV path. MP3 read needs libsndfile built with **mpg123** — present by default
  via vcpkg's `mpeg` feature (Windows x64) and Homebrew's `mpg123` dep (macOS).
  The Insert-sample dialog filter (`SStdMixerView::ctInsertSample`) lists the
  audio extensions; drag-drop already accepts any path. Import is decode-only and
  dependency-satisfied on both targets — unlike MP3 *export*, which needs the
  user-provided libmp3lame binary. Gate: `qxa.mp3_sample_import` (RMS
  discriminator over a committed MP3 fixture; MP3 decode is not byte-cmp-safe
  across mpg123 versions, and carries a small decoder delay).
- **libvorbis / libvorbisenc** — OGG Vorbis export (all platforms)

### Time-stretch / pitch-shift (proposal 27 M5 — in-house default)
- **`twPagedVocoder`** (`tw/sources/twpagedvocoder.h`) — the DEFAULT
  `twGrainSource` backend since proposal 27 M5. An in-house phase vocoder:
  identity phase-locking with prominence gating (the noisy-material comb/metallic
  fix), keyframed phase re-anchoring on a fixed grid ∪ source onsets (transient
  preservation), a transient-preserving time map, and streaming block renders —
  memory is O(pages), not O(clip × variants), and nothing is materialized at
  load. No third-party dependency: analysis is incremental (a lazy windowed FFT
  over resident PCM), so it needs no spectral sidecar. This is the path all
  file-backed clips take unless overridden.
- **Rubber Band Library — REMOVED 2026-07-26** (requester decision). It had
  been the optional reference backend since M5; the vocoder was already
  load-bearing, so removal cost no capability. **The GPL v2+ obligation it
  carried is lifted** — no GPL-licensed code is linked anymore. The
  `warp.pcm` params-blob backend byte `1` stays reserved for the retired
  path (historical cache keys must never alias). The legacy overlap-add
  (`TW_STRETCH_BACKEND=ola`) remains the dependency-free reference.

### Platform-Specific Audio Backends
- **Windows:** WASAPI (SDK: ole32, mmdevapi, avrt, …); MinGW 13.1
- **Linux:** ALSA (libasound)
- **macOS:** CoreAudio

### Optional
- **libmp3lame** — MP3 export (user-provided binary in app directory)
