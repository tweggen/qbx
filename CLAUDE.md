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

## Plugin Hosting (proposal 08 — M0..M5 executed 2026-07-26)

CLAP audio-effect plugins are scanned, inserted per track, heard in the signal
path, saved with the project, and kept as a reloadable placeholder when the
plugin is not installed. **Design:** `plan/proposed/08_PLUGIN_HOSTING.md`;
**what was built and in what order:** `plan/todo/08_PLUGIN_HOSTING_EXECUTION.md`;
**the invariants that matter:** `smaragd/tw303a/plugins/CONTRACT.md` (18 of them)
and `smaragd/main/pluginui/CONTRACT.md`. VST3 (M6) and macOS bring-up (M7) are
open.

### Layers

| Piece | Where | What it is |
|---|---|---|
| ABI | `tw/plugins/twplugin.h` | `twPlugin`: `prepare`/`process`/`reset`, params, opaque state blob. Format-agnostic and deliberately narrow. |
| CLAP backend | `plugins/src/twclapmodule.{h,cc}`, `twclapplugin.cc` | DSO load (`LoadLibraryExW` / `dlopen`, macOS bundles → `Contents/MacOS/<base>`), `clap_entry`, modules interned by path. Maps audio-ports / params / state / latency / gui. |
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
not guessed at.

### Knobs

| Knob | Effect |
|---|---|
| `TW_HAVE_CLAP` (CMake, **PRIVATE** to `tw_plugins`) | Set when `smaragd/third_party/clap/include/clap/clap.h` exists (a git submodule, fetched by `ensure_submodules()` in `_env.sh`). Without it the build compiles, warns, and skips the CLAP half of `plugins_test`. It must never enter a public `tw/plugins/*.h` — that would be ODR/ABI skew. |
| `<configDir>/plugincache.json` | The scan cache (next to `smaragd.ini`). One record per module: path, size, mtime, scanner version, `ok`/`failed`/`timeout`, and the descriptors found. Delete it, or use Rescan with *force*, to clear sticky failures. |
| `smaragd_pluginprobe[.exe]` | The out-of-process probe; the **app** supplies its path (next to the exe; inside `Contents/MacOS` on macOS). Absent ⇒ the scan falls back in-process and logs a warning — safe against a corrupt file, not against a plugin that crashes on instantiation. |
| `plugins/searchPaths`, `plugins/scanOnStartup` (`SOpt`) | Edited on Edit → Options → Plugins. Defaults from `twPluginSearchPaths::defaults(format)`: Windows `%CommonProgramFiles%\CLAP` + `%LOCALAPPDATA%\Programs\Common\CLAP` + `CLAP_PATH`; macOS `/Library/Audio/Plug-Ins/CLAP` + `~/Library/…`. |
| `clap_probe` (target, not a gate) | `plugins/tools/clap_probe.cc` — loads a real third-party `.clap` with the production loader and prints the factory contents. |

Only `*.clap` is scanned on purpose: a `.vst3` found before M6 would be probed,
fail, and be cached as a permanent failure that M6 would have to force-clear.

### Testing without installing anything

`plugins/tests/twtestclap.c` is a real 2-in/2-out CLAP module built from this
repo as `twtestclap.clap` and copied next to the binary. Two entry points:
`tw.test.clap.gain` (`out = in * gain`, plus a "report block size" mode that
writes the frame count it actually saw) and `tw.test.clap.stereoskew`
(`out[0] = in[0]*0.5*gain + in[1]*gain`, `out[c>=1] = in[c]*0.5*gain`) — the
cross-channel term is what makes a silent second input visible in a mono render.
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
- **libsndfile** — WAV export (all platforms)
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
