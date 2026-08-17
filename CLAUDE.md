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
- `SMARAGD_AUDIO_BACKEND=capture|null|default` picks the audio backend at
  RUNTIME, ahead of the platform choice. **`capture` is the default for a
  `--test-case` run** (main.cpp sets it before SApplication exists, unless it is
  already set): it pumps the render callback on a real-time paced clock and
  keeps every frame in memory, which is what makes the PLAYBACK path assertable
  (`<dump-playback-capture>` writes the recording out as a 16-bit WAV and
  assert-source-position decodes it). It also stops a headless suite from
  opening the real output device ~90 times.
- `SMARAGD_MIDI_BACKEND=winmm|coremidi|alsaseq|capture|null|default` picks the
  MIDI ports the same way, ahead of the platform choice (proposal 37 P7a,
  `tw/devices/midi_output.h`). **`capture` is the intended default for a
  `--test-case` run** — it records `{hostTimeNs, port, bytes}` in memory and
  nothing else, so a MIDI-out assertion is measured against the AUDIO capture
  backend's independent block log (`CaptureBackend::frameAtHostTime`, host time
  → project frame, piecewise linear) rather than against the pump under test.
  Unlike the audio variable it is read at every `createMidiOutput()` call, and
  `createMidiOutput("winmm")` names a backend explicitly. MIDI is emitted at
  PLAY time by `MidiOutScheduler`'s Qt-free thread — never at freeze time, for
  exactly the reason level meters are not computed there.
- `SMARAGD_CAPTURE_SPEED=<float>` multiplies the capture backend's pacing (4.0 =
  four times faster than real time) for a smoke run. The pacing is real time by
  default ON PURPOSE — a clock that waited for the readahead would mask exactly
  the races and underruns the playback cases hunt.
- `SMARAGD_RENDER_TIMEOUT_MS=<ms>` overrides `SRenderAction`'s render watchdog
  (default 30000). It is a **wall-clock budget for one render**, so on a busy
  machine it fires on a perfectly healthy one — measured under `ctest -j8`, a
  render advancing steadily (a page every ~1.6 s rather than ~0.03 s) was killed
  at **96 % done**, a second short. `<render>` appears 147 times across the qxa
  suite, so this one constant is what caps the usable `-j`; the suite therefore
  sets it to 180000 and leans on CTest's per-test `TIMEOUT` as the hang guard.
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
└── proposed/             # Numbered proposals 02..37; highlights:
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
    ├── 36_MULTICHANNEL_SIGNAL_FLOW.md (executed 2026-08-16, M0..B8 —
    │                                 the page carries N planar channels; read
    │                                 §4.3-§4.6 and the 28 traps before touching
    │                                 channel width anywhere)
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

The deploy step also copies **`qoffscreen`** into `build/bin/platforms/`, which
`windeployqt` does *not* do: it deploys only the platform plugin the *app* needs
(`qwindows`), while every headless gate runs with `QT_QPA_PLATFORM=offscreen`.
Its absence does not fail fast — Qt blocks in a platform-plugin dialog nobody can
see, so each affected test burns its **entire 300 s timeout at ~0 s of CPU**
instead of passing in ~0.05 s. That has cost real gate runs ten minutes of pure
timeout and reads as a suite problem rather than a missing file. If you build
with `AUTO_DEPLOY_QT=OFF`, copy it by hand:
`cp <QtPrefix>/plugins/platforms/qoffscreen.* smaragd/build/bin/platforms/`.

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
  youtrack https://nassau.youtrack.cloud/mcp \
  --header "Authorization: Bearer perm:<permanent-token>"
```

**`--header` goes last.** It is declared variadic (`-H, --header <header...>`), so putting
it before the positional arguments makes it swallow the server name and the URL as extra
header values, and the command dies with `error: missing required argument 'name'`.

Token: YouTrack → Profile → Account Security → Authentication → New token, scope
*YouTrack*. Requests run with that user's own permissions. **Do not use `--scope project`**
— that writes `.mcp.json` into the repo and would commit the token.

Validate the token *before* wiring it. A bad token surfaces through MCP only as an opaque
connect failure, whereas the REST API answers in one line — `200` good, `401` bad:

```bash
curl -s -o /dev/null -w '%{http_code}\n' \
  -H "Authorization: Bearer perm:<permanent-token>" \
  https://nassau.youtrack.cloud/api/users/me
```

`/api/users/me` succeeds for any valid token regardless of scope, so a `401` there means
the credential itself is rejected — not a permissions or MCP-feature problem. To replace a
token: `claude mcp remove youtrack -s user`, then add again.

### Gates, before every PR

```bash
./build.sh                                   # re-configures: required, see below
python3 tools/check_layering.py              # module boundaries
python3 tools/check_logging.py               # no direct stderr/stdout writes
ctest --test-dir smaragd/build -j4 --output-on-failure     # THE routine gate
```

**Run the suite in parallel. `-j4` is the routine gate.** The cases were audited
for isolation and are safe to run concurrently — see "Why `-j` is safe" below,
and do not weaken any of those properties without re-reading it. There is no CI,
so this suite is the entire safety net and every branch pays it in full; `-j` is
the cheapest available win.

Measured on this repo's usual Windows box (16 logical cores, 16 GB RAM),
107 tests run + 3 disabled, back to back, nothing else of consequence on the
machine:

| Mode | Wall clock | Speedup | Result |
|---|---|---|---|
| serial | 2338 s | 1.00× | 105/107 — 2 crash flakes, see below |
| `-j2` | 1151 s | 2.03× | 106/107 — 1 crash flake |
| **`-j4`** | **791 s** | **2.96×** | **107/107 green** |
| `-j8` | 552 s | 4.23× | 107/107 green |
| serial, repeated later | 2719 s | — | 107/107 green |

Counts reconciled both ways and every time: 89 `.qxa` files on disk = 89 `qxa.*`
tests registered, 110 registered in total, 3 Not Run (Disabled), **107 run**.

The serial run was done twice because the first one was the only run in the set
that was not green, and its two failures were crashes rather than assertion
failures. The second serial run is 16 % slower than the first for no reason
either run can explain, which is roughly the noise floor for wall-clock numbers
on a desktop — do not read small differences in this table as signal.

`-j8` is faster still and was green here, but `-j4` is the recommendation: it
leaves headroom on a box that is also running an editor, a browser and possibly
another worktree's build, and the failure mode of running out of headroom is not
graceful (see the render watchdog below). Scale by RAM, not by core count.

A serial run uses about **1/16 of this machine** — measured, CPU sat at ~6 %
during it. That is the whole argument for `-j`.

- **The re-configure is load-bearing.** The qxa glob in `smaragd/CMakeLists.txt` is
  `CONFIGURE_DEPENDS`; without a configure pass a newly added `.qxa` is never registered
  and `ctest` reports all-green while never having run it.
- **Reconcile the count**: registered vs run vs skipped — `ctest -N` against the run's
  own summary, and do it for the parallel run too. A silently-unregistered case is a
  failure mode this repo has actually hit. On a non-Apple box the expected shape is
  **110 registered / 107 run / 3 Not Run (Disabled)** — the disabled three are the
  macOS-only `au_*` trio.
- **A case that fails once and passes on re-run is not a pass.** Pin it with
  `smaragd/tests/repeat_test.sh <bin> <case.qxa> [N] [workers]`, swept over
  `SMARAGD_REVAL_WORKERS` {1,4,8,16}, before deciding it is a flake. Report it either way.
  **Run `repeat_test.sh` from `smaragd/tests/cases/`** or it reports 0/N for perfectly
  good cases (the fixture paths are CWD-relative).

#### When to run it SERIALLY instead

`-j` is the routine gate, not the only one. Drop back to plain `ctest` (no `-j`) for:

- **Flake hunting.** Reproducing an intermittent failure means controlling the
  variables, and background load from 3 concurrent cases is a variable. Same for
  every `repeat_test.sh` sweep — those exist to separate a real race from noise,
  which they cannot do while the box is also running seven other cases.
- **DSP-sensitive cases** (`grain_*`, `exact_*`, `stress_*`, `warp_*`) when the
  change touches page freezing, invalidation or predecessor chaining. Run these
  first and separately: they are the ones most able to be perturbed, and a
  byte-exact `cmp` gate deserves an uncontended run.
- **Anything timing-shaped you are actually investigating** — latency, underruns,
  readahead behaviour. Under `-j` the rest of the suite IS the load.

**A `-j` failure is not automatically an isolation bug.** Two tests assert a
wall-clock LATENCY BOUND, so they measure the machine rather than the code, and
both carry `RUN_SERIAL` (CTest runs them alone within the invocation):

| Test | Asserts | Why it moves |
|---|---|---|
| `twlog_test` | one non-blocking `TW_LOG` from a pretend-RT thread completes in **< 2000 µs** while 4 threads contend the logger lock | 137–191 µs on an idle box; **4 000–51 000 µs at ~100 % CPU**, failing 6 runs in 10 — same binary, no code change |
| `qxa.log_dock_scale` | **no single event-loop pump exceeds 50 ms** while the log dock drains 300 k records | a stall cap is a latency bound on the GUI thread; same shape |

`RUN_SERIAL` only excludes *other tests in this ctest run*. It cannot protect
them from load outside it — a second worktree building, or another agent's suite.
If either fails, **confirm the box is idle before treating it as a regression.**

#### Two known crash flakes — NOT parallelism (they show up SERIALLY)

Measuring the curve above turned up rare crashes that predate and are unrelated
to `-j`. Recorded here so the next person does not re-diagnose them, and does not
mistake them for an isolation bug:

| Case | Shape | Seen |
|---|---|---|
| `qxa.clip_properties_actions` | `***Exception: SegFault` | 1 of 2 serial runs |
| `qxa.split_plain_screenshot` | script prints `PASS`, process then exits non-zero — a crash during **teardown**, after every action and assertion succeeded | 1 of 2 serial runs, and `-j2` |

They are not a `-j` problem: they appeared in the **serial** run and both passed
in the green `-j4` and `-j8` runs. Neither reproduces in isolation —
`split_plain_screenshot` 80/80 (including 25 with `SMARAGD_SIDECAR_DIR=off`) and
`clip_properties_actions` 15/15 — so they need the full-suite context. Root cause
is **not** established; treat them as open. `split_plain_screenshot` is itself a
crash regression test for the split-then-repaint path under live playback, and
this failure is a delayed-destruction variant of exactly what it guards.

**`repeat_test.sh` cannot see the second shape at all.** It judges a run by
grepping stdout for `^PASS - `, so a case that passes and *then* crashes on exit
counts as a pass, while `ctest` — which judges by exit code — fails it. When you
are chasing a teardown crash, loop on the exit code instead.

#### Why `-j` is safe (audited, not assumed)

Every way two concurrent cases could interfere, and the evidence. Treat this as a
contract: a change that breaks one of these breaks the parallel gate.

| Shared thing | Verdict |
|---|---|
| **Per-case artifacts** | Isolated. Each case gets `--test-output-dir build/test-output/<case>`, and `render` / `screenshot` / `dump-playback-capture` / `assert-audio-*` / `sidecar-root` all resolve *through* `SApplication::testOutputDir()` and **refuse to run when it is unset** — none of them can fall back to the CWD. |
| **The shared `WORKING_DIRECTORY`** | Clean. All 89 cases run from `tests/cases/`, but nothing writes there: every `path=` in a `.qxa` is either a committed read-only fixture under `tests/`, a plugin module name, or an explicit `../../build/…` target. `git status tests/` stays clean across a full run. |
| **`.qxp` save targets** | No collision. Nine cases save into the build root; the names are unique per case (`au_missing_resave`, `au_slot_resave`, `clip_properties_actions`, `exact_stretch_roundtrip`, `plugin_missing_resave`, `plugin_remove_restores_param`, `plugin_slot_resave`, `takes_roundtrip`, `warp_anchors_roundtrip`) and each is written and read back **by its own case only**. No case consumes another case's artifact. |
| **The sidecar (QAF) store** | **This was a real bug, now fixed.** One shared per-user cache dir, and **80 of 89 cases use the same `test_sawtooth.wav`** → the same content hash → the same aspect keys, so concurrent stores of one key are the normal case, not the exotic one. The writer used a fixed `<path>.tmp`, so two processes truncated and interleaved into one temp and one published the mixture. Only the QAF **header** is CRC-protected — a torn payload of the right length passes the reader's bounds check and feeds wrong analysis data (onsets / f0 / warp.pcm) into the engine. The temp is now `<path>.<pid>.<seq>.tmp`; see `tw303a/sidecar/CONTRACT.md` inv. 2. Note the hazard is **latent**: it only bites when a key is cold, i.e. on the runs right after an aspect-version bump or a cleared cache — exactly when nobody is expecting it. |
| **`plugincache.v<n>.json`** | Harmless. Written on every run (`plugins/scanOnStartup` defaults true), but through `QSaveFile` — write-to-temp-then-rename, so a concurrent write is a lost update at worst, never a torn file. Verified live: parsed as valid JSON repeatedly while four `smaragd.exe` processes rewrote it. A lost update costs a re-probe, and records are keyed on path+size+mtime so it cannot manufacture a false failure. |
| **`smaragd.ini`** | Harmless. Qt takes a `QLockFile` around `QSettings` writes, and a headless run does not write it at all — mtime unchanged across a full suite. |
| **`smaragd.log`** | Not shared. `--test-case` runs deliberately take **no file sink** (`main/shell/src/main.cpp`), which the code already justifies by naming `ctest -j`. |
| **Wall-clock latency assertions** | Not isolation bugs — see the table above. `RUN_SERIAL`. |

Deliberately **not** isolated per test, with reasons:

- **The sidecar store stays shared.** Once the temp name is per-writer, sharing is
  safe, and it is worth keeping: a per-test store would recompute every analysis
  in every case and make the gate substantially slower for no correctness gain.
- **No `RESOURCE_LOCK` is used anywhere.** Nothing left needs mutual exclusion —
  every remaining shared resource is either per-case, atomically written, or
  read-only. `RUN_SERIAL` on the two latency tests is a different mechanism for a
  different reason (they need an idle box, not exclusive access to a file).

**The thing that actually caps `-j` is a wall-clock watchdog, not isolation.**
`SRenderAction` bounds one render with a 30 s wall-clock budget, meant to catch a
render that has HUNG. Under load it instead catches renders that are merely slow:
measured at `-j8`, a render was advancing steadily — a page every ~1.6 s instead
of the usual ~0.03 s — and was killed at **96 % done** (2 764 800 of 2 880 000
samples), about a second from finishing. Before this was addressed, a `-j4` run
taken while three other worktrees were also running suites produced **19 failures,
every one of them this timeout**, in a single contiguous 3.6-minute window; re-run
afterwards on a quieter box, all 19 passed. So the failures were a load artifact,
not a case-isolation bug — but with `<render>` appearing 147 times across the
suite it is the dominant `-j` risk. The qxa cases now run with
`SMARAGD_RENDER_TIMEOUT_MS=180000` and a CTest `TIMEOUT 600`, which keeps a real
hang bounded while letting a slow-but-progressing render finish.

**If you see a wave of `Action render (#N) failed to apply` failures**, that is
this, and the diagnosis is load rather than the change under test. Check
`render timeout after` in the output and whether the failures cluster in time
rather than by case.

**Memory is the other limit.** Each case is a whole
`smaragd.exe` that pre-allocates a 512 MB `CapturePagePool` and spawns 8
revalidation workers — **~720 MB committed / ~250 MB working set per process,
measured**. On a 16 GB box `-j4` is comfortable and `-j8` is tight; scale by RAM,
not by core count. Do not lower `SMARAGD_REVAL_WORKERS` to buy headroom — the
worker count is part of what the race-hunting cases exercise.

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
| Its page is N CHANNELS WIDE since proposal 36 B4, and the meter is N LANES since B8 | `twLevelSample` / `twScanSpan` / `twMeterBallistics` are still scalar **by type** — a lane IS one span, a `twLevelSampleSet` is N of them, and only the probe (which channel) and the widget (how to draw N bars) learned about channels. The probe reports `min(wantLanes, page->channels())` — the width of the PAGE IN HAND (§4.4), never the tap's declared width, because an insert-less `twPluginChain` forwards its input page verbatim and its silence pages are width 1. §4.5's width check in `twLevelProbe::resolvePage_` still applies to every rung: a cached page whose `channels()` no longer matches its producer's declared width is a MISS (the meter decays), never audio, because reading `channelPtr(1)` of a stale one-channel page is an out-of-bounds read. |
| `outputLatencyFrames` is in DEVICE frames at the DEVICE rate | The locator counts PROJECT frames. `SApplication::meterLatencyFrames()` scales by `projectRate/deviceRate`; skipping that is a ~9% error for 44.1 k on a 48 k device. Applied ONCE in the pump so all meters share one position. |
| Ballistics live on the UI thread, driven by wall-clock dt | Frame-rate independence (one 1 s step == 100 × 10 ms steps) is asserted by `metering_test` and is the reason they are not in the engine. |
| `meterTimer_` is NOT a fold into `pumpLocator` | `pumpLocator` only works when the position changed and stops the instant playback stops. Meters need a tick at a static position (to decay) plus a ~8 s tail, or the bars freeze mid-level. Not started during an offline render; started while recording. |
| A page miss must DECAY the meter | `advanceTo()` returning false → `idle()`. A dropout then reads as a fast fall, never as a frozen bar. Nothing here may block, wait, or create a demand. |
| Stale-but-frozen pages are deliberately ACCEPTED | Playback serves exactly those (proposal 16), so rejecting them would make the meter disagree with the ear while an edit is absorbed. |
| HOW MANY LANES a meter shows is the MOUNT's decision (proposal 36 B8) | The **track head** and the **transport master meter** show at most **two** (`SLevelMeter::MONITOR_LANES`), dividing the fixed 8 px bar among them. That follows the device rule `twSpeaker` states — `L = ch0; R = (width >= 2) ? ch1 : ch0`, the rest computed in full and dropped at the device — because a 120 px control column has ~13 px of slack and six lanes in it would be 1 px each. The **Track Detail dock** shows EVERY channel and grows its short axis to do it (`setGrowWithLanes`); it is the answer to "where do I see channel 4". The cap is ANNOUNCED, never silent: `describe()` reports `lanes` and `width` as separate fields and the tooltip names the width and points at the dock. |
| The width a meter uses comes from the TAP, not from `SProject` | `getRootComponent()->getOutputChannels()`. Since B4 a track has no bus count of its own — it has the project's width — so asking the component keeps the meter and the audio reading ONE number, and it is the same number §4.5 compares a cached page against. |
| `twAspectMetadata` stays unclaimed | `freezePage` already stores `validAspects = twAspectAll`, so that "peak levels" bit is already set and already meaningless. Claiming it would drag metering into the demand system for nothing. |

**The "legacy pull does not see a gain change" hole is CLOSED** (proposal 37
P3a). It used to read: the legacy pull does not observe a track-gain change made
after a position was first frozen, because `twStreamingLatch::copyData` gates its
cached page on the **`twPluginChain`'s** content epoch — so a gain had to be set
BEFORE first probing a position, and `meter_postfader.qxa` uses two tracks at
different gains rather than changing one track's gain twice.

The fader is no longer inside `twTrackMix`. It is **`twGainStage`**, wired
between the plugin chain and the rewire (`twTrackMix → twPluginChain →
twGainStage → twRewire`), so the epoch that guards the rewire's cached input page
is exactly the one `set-track-volume` bumps, and a gain change after a freeze is
observed on every path. Gate: `meter_gain_after_probe.qxa` (probe, set the gain,
probe the same position again). `meter_postfader.qxa` keeps its two-track shape,
which is a good case either way. Measured honestly: the new case also passes on
the pre-move binary at the 36-B4 integration tip, so B4's collapse had already
made the caveat inert in practice — P3a is what makes it structurally impossible.

There is now ONE volume-fader curve, `app/timeline/sfadercurve.h`. The Track
Detail dock's slider used to be wired to nothing and to map `value = dB*10`,
disagreeing with the arranger's `VOLUME_CURVE_EXPONENT = 0.5`; both now share the
curve and commit through `SSetTrackVolumeAction`.

**And the fader is POST-FX** (proposal 37 P3a, design D5). `twGainStage`
(`tw/mix/twgainstage.h`) is one wide component per track between the last insert
and the rewire; `twTrackMix::setTrackGain` is a no-op kept until P5. An insert
therefore sees the UNFADED signal, which is what an instrument's output needs and
what every reference DAW does. A linear insert cannot tell the two orders apart,
so the ordering is gated by a hard CLIPPER (`tw.test.clap.gain` param id 2) in
`fader_post_fx.qxa` against a closed form, never by a byte compare — and the
committed goldens are byte-identical across the move by construction, because at
0 dB the stage does no arithmetic at all and no golden combines a non-unity fader
with a plugin. Mute stays STRUCTURAL (the parent nulls the plug); the gain
stage's ramped audio mute exists but is unwired until P5's `self:Muted` lane.

Gates: `ctest -R metering_test` and the qxa cases `meter_levels` (per-second RMS
of the ramped-sawtooth fixture, the miss/silence path, the density rules via the
REAL head built off screen, plus PNG grabs — the only coverage of
`SLevelMeter::paintEvent`) and `meter_postfader`. Since B8 `meter_levels` also
carries the LANE gate, and it is a **pair**: `test_stereo.wav`'s 6 dB ladder must
show two lanes that differ, and `test_sawtooth.wav` — two channels holding the
SAME audio (trap 22) — must report two lanes whose delta assertion is REJECTED.
Only a real per-channel meter passes both. Never make a lane claim over
`test_sawtooth.wav` alone; that is the defect that made `channel_assert_dupmono`
incapable of detecting the sink going wide. The head is also grabbed at 150/100/
60/40 px lane heights against both the 120 px and a 200 px column.

### Waveform previews fold every channel (proposal 36 B8)

A preview probe is the signed min/max envelope of its window over **all**
channels — the union of the per-channel envelopes — in
`SObject::straightCalcPreviewData` and `SCut::ensureCapturePeaks` alike. It was
channel 0 alone, which was harmless while nothing above width 1 reached a sink
and is a lie now. The drawn waveform stays ONE lane on purpose: `preview_t`,
`swaveformdraw`, `SCut::getPreview` and every inline renderer are single-envelope
by type, and an arranger clip lane has no room for six stacked waveforms.
Per-channel LEVEL is the meter's job. The fold is key material, so
`twAspect::PreviewPeaksVersion` is **2** and every v1 sidecar orphans on sight
(gated in `sidecar_test`: an old file must MISS and be deleted, not be adopted).

**A Preview ASPECT PAGE is a different thing and is not a probe array.**
`CapturePageData::data` holds float samples at ~1 kHz written by
`CaptureRevalidator::dispatchRecomputation`, with no probe count, hop or duration
attached, and **nothing in the tree reads that payload** — `SCut::getPreview`
uses the page's existence as a readiness signal only. `SPlainWave::getPreview`
used to `reinterpret_cast` it to `preview_t*` (proposal 36 trap 26); that branch
was unreachable, because only `SCut` ever calls `scheduleRevalidation`, so a
non-`SCut` object's `currentPage_` is always null. B8 removed it rather than
"fixing" the cast: a page with no geometry cannot answer a
`(start, length, nProbes)` question whatever its element type.

### The render dialog DISPLAYS the channel count; it does not override it

File → Render shows the project's width read-only. `RenderParams::channels` keeps
meaning what B5 built — a number, or 0 for "ask the graph". An override was
considered and rejected: reducing 6 channels to 2 needs channel roles and a fold
law, which proposal 36 §8 names as a non-goal, and the obvious candidate (the
device rule's "first two, rest dropped") is a listening compromise that must not
be applied silently to a delivered file. One authority for the width: the project.

## Multichannel signal flow (proposal 36 — M0..B8 executed 2026-08-16)

A project has a **channel count** (1/2/4/6/8, `<SProject channels='N'>`, default 2
for legacy files) and it is carried end to end: clip → track → master → file. The
design, the twenty-eight traps eight milestones paid for, and every licensed
golden re-freeze are in `plan/proposed/36_MULTICHANNEL_SIGNAL_FLOW.md`. **Read
§4.3–§4.6 before touching channel width anywhere.**

**The frozen page carries its channels.** `twOutputPage` is planar with a
**constant `CHANNEL_STRIDE == FRAME_CAPACITY`**, `channels()` is immutable after
allocation, and the sample buffer is **private** — `channelPtr(c)` and
`channelFrames()` are the only ways in. This replaced the old "one mono page per
component, so N channels are N component instances" rule, and with it the
per-bus instantiation: **a track is one `twTrackMix` + one `twPluginChain` + one
`twRewire` of width N**, and a slot is one `twPluginInsert`.

| Rule | Where | Why it is that way |
|---|---|---|
| Width 1 keeps **today's code path** — the same call, not an equivalent one | `freezePage_nolock` forks on `page->channels()` | It is what makes the byte-exactness gate meaningful through the whole rewrite |
| Width > 1 **must** override `renderPageWide()`; the base **refuses and logs** | `twComponent` | A per-channel loop over `calcOutputTo` would advance a cursor per channel and fill channel 1 with the **next page's** audio. Never a `Q_ASSERT` — this build compiles those out |
| A plug pull yields channel `min(latchIndex, page->channels()-1)` | `twStreamingLatch::copyData` | One clamp, one place. A wide component reads its bound **pages** directly instead |
| Act on **`page->channels()`**, never a producer's declared width | everywhere | The tree launders pages between components; the width you may act on is the width in your hand |
| A stale page whose width ≠ its producer's declared width is a **MISS** | `AudioEngine`, `twLevelProbe` | Proposal 16 serves stale pages to the RT thread; `channelPtr(1)` of a width-1 page would be an **out-of-bounds read on the audio thread** |
| **Monitoring is stereo; rendering is not** — `L = ch0; R = (width >= 2) ? ch1 : ch0` | `twSpeaker` (device only) | Requester's decision. Render and monitor share **no** code, so a 6-channel project renders six channels *and* monitors in stereo |
| Meters show **two lanes on the track head**, every channel in the Track Detail dock | `SLevelMeter` | Six lanes do not fit a 120 px column; capping at the pair you can actually hear keeps the head honest about the monitor path. The cap is announced, not silent |

**Two things that will mislead you if you do not know them:**

- **`test_sawtooth.wav` is a two-channel file whose channels are byte-identical**,
  and 80 of the ~90 qxa cases use it. It therefore **cannot gate any channel
  claim** — a render made from it has equal channels whether the engine is wide or
  not. This is what made `channel_assert_dupmono` incapable of detecting the very
  thing it was built to detect. Use `test_stereo.wav` or `test_channels4.wav`
  (6 dB ladders, generated by the committed `gen_channel_fixture --verify`).
- **`L != R` on a file is now legitimate**, where the old contracts forbade it.
  The equal-channels-by-construction era ended at B5. Proposal 37's instrument
  and automation phases were written BEFORE it and read channel 0 only; that
  caveat was retired on 2026-08-17 by `instrument_stereo_render.qxa` (the four
  generator mapping rows, per channel, from a file) and `automation_stereo.qxa`
  (a `self:Volume` ramp and a `param:` step, on both channels).

**Both goldens (`smaragd/tests/goldens/`) are the byte gate for all of this** and
have been re-frozen exactly twice, each under a licence recorded beside the case:
at B4 (a mono project's stereo plugin now folds — the old bytes were a *saturating*
render of a project whose width reached no track) and at B5 (mono became a
one-channel file; stereo's channel 1 became real audio).

## Event clips (proposal 37 P1 — executed 2026-08-15)

MIDI is in the model: `SMidiSequence` (content) + `SMidiCut` (window) +
`SLink::timebase` (placement), the verbs to edit them, and a per-track event
feed the instrument slot will read in P3b. Nothing SOUNDS yet — an event clip
on a track without an instrument is inaudible, not rejected (design D3).
Design: `plan/proposed/37_MIDI_INSTRUMENTS_AUTOMATION.md` §3.1–§3.4.
Invariants: `main/objects/midi/CONTRACT.md` (11 of them), `main/objects/track/
CONTRACT.md` inv. 5b/5c, `docs/contracts/POSITION_DOMAINS.md` rule 7.

**Read this before touching anything positional — the obvious design is wrong.**
Events are stored in MUSICAL TICKS (PPQ 960) and so is the window; frames are
DERIVED. Two of the three code studies recommended frames-now and were
overruled on industry evidence: recorded MIDI that does not follow a tempo
change is a defect users hit in the first hour, and Ardour ≤ 6 is the
cautionary tale that forced their 7.0 rewrite.

| Thing to know | Why |
|---|---|
| `twTempoMap` (tw/events) is THE tempo authority; `SProject::getBPMTempo()` is a derived view and `bpmTempo_` is gone | Tempo is stored as SMF's own unit, µs per quarter (an integer), so BPM and the map cannot disagree. A stored `60/bpm` seconds-per-beat and a stored µs/quarter differ in the tenth microsecond, which lands on a frame boundary in a long project. |
| The tick→frame conversion happens EXACTLY ONCE per value, inside `SMidiCut::rebuild_nolock()` | Two callers converting independently is how a rounding difference becomes an off-by-one clip edge. `getDuration()`, `loopLength()`, `startOffset()` and the frame-domain event table are all derived there, by multiplying an exact tick `Fraction` by the map's exact frames-per-tick and flooring once. |
| `set-tempo` is the ONLY tempo write, and it is an ACTION | It re-derives `startTime` for every `timebase=beats` link in the project (nested containers and assets included) — so a MIDI clip at bar 5 stays at bar 5 while audio does not move — and being an action is what keeps undo exact by LIFO. The two direct `setBPMTempo()` writes (the ruler dialog, the transport box) are gone. Gate: `grep -rn "bpmTempo_ =\|setBPMTempo(" main/` hits only the verb and the loader. |
| An event clip goes into `STrack`'s `twEventClipSet`, NEVER into the bus mixers | A MIDI clip has no page to freeze. Inserting one as a `ClipEntry` costs a dummy freeze per page per clip AND makes `twView::getComponent() returned nullptr` fire once per freeze forever. The absence of that log line is the only observable difference between the two routings — hence `assert-log … maxCount="0"` in `midi_clip_render_silent`. |
| `objects/track` has NO edge to `objects/midi` | The track consults MIDI-ness through `SObject::contentKind()` and `SObject::resolveEventClip()` — both on the base class for exactly this reason. `objects/midi` sits at the RANK of `objects/cut`: a second window/content pair, not a layer above one. |
| Split is NON-DESTRUCTIVE: the window gates, the sequence is never edited | A note straddling the split keeps its ORIGINAL duration in the head; the head's window end SYNTHESISES the note-off; the tail never re-attacks it (a note-on before the window reaches a consumer only through the chase set). A content-editing `split-notes-at` is a later verb. |
| A note-off exists only inside a `collect` | Notes are stored WITH their length, so nothing in any table is a note-off. `assert-midi-events kind="noteoff-synth"` runs a real collect over the clip's window PLUS ONE FRAME — windows are half-open and a clip-end release lands on the boundary, i.e. in the window that STARTS there (events/CONTRACT inv. 8–9). |
| The track FEED is rebuilt on every read | `STrack::eventFeed()` merges its own clip set with every child track that bubbles events up (design §3.2.1). Solo is GLOBAL, so a dirty flag would have to be poked from anywhere in the project — which is the coupling `ssolorules.h` exists to avoid. The merge OBJECT is stable; only its source list is recomputed. |
| `serializeSelfAttributes` must not hold `mutex()` across the base call | `SObject::serializeSelfAttributes` calls `getDuration()`, which takes the same mutex, and `std::mutex` is not recursive. The failure is silent: the save simply never finishes. |
| Notes are persisted INLINE (`<events><e …/></events>`), sorted on write | Note data must never be able to go missing the way a sample file can, so an imported `.mid` is materialised on the first save and the file is never consulted again. Unknown kinds — and every meta payload — round-trip verbatim. |

Gates: `midi_clip_roundtrip` (import → save → load → export is
BYTE-IDENTICAL; legitimate only because twSmf has one canonical spelling and
`tests/midi_multitrack.mid` was authored by it — `midi_fixture_authoring`
regenerates it), `midi_clip_edit_verbs`, `midi_clip_tempo_remap`,
`midi_clip_render_silent`, `midi_folder_feed`, plus `action_roundtrip_test`.

## MIDI output (proposal 37 P7 — executed 2026-08-15)

A track can send its event feed to a MIDI port. The device layer is
`tw/devices` (P7a: `MidiOutput`/`MidiInput`, WinMM/CoreMIDI/ALSA-seq/capture/
null, `MidiOutScheduler`); the app half is `SMidiOutPump` in `main/shell`
(P7b). Design: `plan/proposed/37_MIDI_INSTRUMENTS_AUTOMATION.md` D6 and §4.6.
Invariants: `tw303a/devices/CONTRACT.md` inv. 10-18, `main/shell/CONTRACT.md`
inv. 7-8, `main/objects/track/CONTRACT.md` inv. 9-10.

**Read this before touching MIDI-out — the obvious design is wrong, for exactly
the reason the level meters' was.** MIDI is emitted from the PLAYHEAD, on the
main thread, at PLAY time. Never at freeze time: pages are frozen ~1.4 s ahead
of the playhead by the readahead, and by renders that have no playhead at all,
so a freeze-time MIDI-out would spray a whole arrangement at the user's
hardware synth with the transport stopped.

| Thing to know | Why |
|---|---|
| `SMidiOutPump` is a 20 ms `QTimer` with a 250 ms lookahead, started by `setPlaying(true)` | It is a sibling of `meterTimer_`, never a fold into it: the meters keep ticking after a stop so the bars can decay, whereas MIDI-out must go silent — and send its all-notes-off — at the instant the transport does. |
| The clock anchor is re-taken on every position PUBLICATION, not every position CHANGE | `twSpeaker` defers the device start until the readahead is primed, so before the first callback the playhead sits still at the locator. Measured: anchoring on a change put the first note of a run **59 ms early**. `SApplication::locatorPublishSeq()` is the counter the RT thread bumps next to the position store. |
| The published position is one device buffer AHEAD of the frame just delivered | `twSpeaker` publishes `engine->currentPosition()` AFTER the pull, so the frame handed over at that instant is `published − bufferFrames`. Skipping the correction is ~21 ms at 1024 frames / 48 kHz. |
| Output latency reuses `meterLatencyFrames()` verbatim | It already converts DEVICE frames at the DEVICE rate into PROJECT frames (proposal 34). `dueHostTime = hostTime(playhead) + deviceOutputLatency − midiOutLatency − globalOffset − trackOffset`. |
| The pump reads the track FEED (`STrack::eventFeed()`), not its own clip set | So a folder parent's port carries its children's patterns, with the channel remapped to the parent's — the drum-machine-on-the-folder case (design §3.2.1). A child with its own port stops bubbling, by the same `auto` rule. |
| De-dup is a monotone per-track FRONTIER plus its loop iteration | Windows are contiguous and never overlap, so it subsumes the design's `(clip key, event ordinal, loop iteration)` key set — and survives an edit that renumbers ordinals mid-flight, which a key set would not. |
| `midiOutPort` is a portable NAME; `midiOutChannel` is 0-BASED | The id `open()` wants (a WinMM index, a CoreMIDI uniqueID) means nothing on the next machine, so `SSettings` maps `midi/portId/<name>`. The channel matches `twEvent::channel` and `add-note channel=`, so the scripting API speaks one convention; `-1` = "as authored". |
| `offsetMs` is signed, ±500, POSITIVE = send EARLIER | Outboard gear whose audio return arrives late is compensated so its audio lands on the grid. An event whose shifted due time falls before the run start is CLAMPED — you cannot send before the transport started. |
| Nothing below the ring may touch Qt | `MidiOutScheduler`'s sender is a plain `std::thread`; a Qt signal from it would make Qt adopt the thread and deadlock the join at teardown. The pump is the ring's SINGLE producer and it is the main thread. |

**Measurement is independent of the thing measured** (design review #12), which
is what makes the gates worth anything: the capture MIDI port records
`{hostTimeNs, port, bytes}` and deliberately NOT the due time it was asked for,
while the AUDIO capture backend records `{hostTimeNs, firstFrame}` per delivered
block. `assert-midi-out` maps through the AUDIO log
(`CaptureBackend::frameAtHostTime`) and subtracts the device latency, so `at` is
"the project frame whose audio was being HEARD when this message left".
`SMARAGD_CAPTURE_SPEED` must be 1.

Gates: the qxa cases `midi_out_capture`, `midi_out_chase_and_stop`,
`midi_out_loop_wrap`, `midi_out_offset_and_folder` (all `RUN_SERIAL` — they
assert wall-clock latency), `midi_out_render_silent`, `midi_out_backend_reject`
and `midi_options_page`, plus `devices_midi_test` and `action_roundtrip_test`.
Measured error across the playback cases: **−98 … +581 frames** against a
4096-frame budget. NOT gated: WinMM jitter against real hardware, CoreMIDI /
ALSA-seq, virtual-port creation on Windows (WinMM has no such concept — a
loopMIDI-style driver appears as an ordinary device), `CAPTURE_SPEED ≠ 1`,
sysex (refused by the ring rather than truncated; P9).

## Automation (proposal 37 P5 — executed 2026-08-16)

A lane on a track, a plugin slot or a clip window is edited by seven undoable
verbs, persisted inline with its owner, snapshotted as an immutable
`twAutomationCurve`, and consumed **at freeze time**. Design:
`plan/proposed/37_MIDI_INSTRUMENTS_AUTOMATION.md` D5 / §3.3 / §3.4 / §4.5.
Invariants: `tw303a/mix/CONTRACT.md` inv. 19-23, `tw303a/plugins/CONTRACT.md`
inv. 15 + 41-44, `main/objects/track/CONTRACT.md` inv. 11-15,
`main/model/CONTRACT.md`.

| Thing to know | Why |
|---|---|
| **A lane is a plain owner-held `QObject`, NEVER an `SLink` child**, serialized inline as `<automation><lane target= mode=><p t= v= c=/>…</lane></automation>` | The project loader orders and resolves on `<SLink>` children only, so an inline child of a known element is invisible to it and an OLDER build ignores it. A lane as an `SObject` would need an id, a link, a load order and a policy for what happens when its owner is dropped — for a breakpoint table with no independent existence. |
| The lane vector lives on **`SObject`**, not on the four owner types | Same argument as `contentKind()` and `resolveEventClip()`: a verb, the serializer and the testkit must reach a lane without knowing which object slice owns it. WHICH targets are legal on WHICH owner is the verbs' business. |
| **`SObject::serialize()` writes NOTHING when there are no lanes** | That is what keeps every project file written before P5 — and every committed golden — byte-unchanged. The same discipline runs through the engine: a NULL curve is the SCALAR path, and at 0 dB unmuted `twGainStage` still does no arithmetic at all. |
| Value domains are the TARGET's own: `self:Volume` in **dB**, `self:Muted` 0/1, `param:<id>` in the plugin's **host-facing** domain (normalized for VST3), `cut:Gain` a **LINEAR** factor | dB for the fader because that is its unit and because Trim's "static value × curve" is then a dB SUM; linear for a clip envelope because a fade-out has to reach EXACTLY zero. `param:` matches `set-plugin-param` by construction. `Rate`/`Stretch` are not automatable (they change duration); `self:Pan` is still absent — the sink has been wide since 36-B5, so what is missing is the pan itself (a clip model that carries one), not somewhere to hear it. |
| A `Linear` segment on `self:Volume` interpolates **linearly in dB** | `twAutomationCurve` interpolates the STORED value and `tw/mix` may not include `app/timeline/sfadercurve.h`. The design's "dB-linear in fader space" is read as "linear in dB, in the fader's space" — the only reading that is implementable and the only one under which Trim is a sum. |
| **A `self:Muted` lane holds AUDIBLE before its first point**, unlike every other lane | Every other lane holds its first point's value there (the universal convention). "Muted from frame 0" is what the STRUCTURAL mute says, and a lane drawn to mute a track at 1 s must not silence second 0. Implemented as an explicit anchor point at frame 0 in the snapshot builder, never as a special case in the consumer. |
| Mute has TWO meanings and they are different mechanisms | The mute BUTTON is STRUCTURAL — the parent nulls the child's input plug — so a track's own output still carries its material and an asset capture of it is not silence. The `self:Muted` LANE is AUDIO: `twGainStage`, post-FX, with a ~1.5 ms ramp at every transition. |
| A `param:` lane becomes **per-chunk, sample-offset `ParamValue` events**, not a `setParam()` call | Chase at offset 0 (pages freeze out of order and on any worker, so what the instance holds is unknown BY CONSTRUCTION), one event per breakpoint inside the chunk, a 64-frame grid on continuous segments for plugins that do not interpolate. With no curves the call is the SAME legacy `process()` it always was. |
| **The invalidation range differs by consumer CLASS** | `twGainStage` is class infinity and pure, so the range is EXACT. A plugin is CLASS 1, so it is `[a, INT64_MAX)`. A `cut:` lane invalidates in the cut's own clip-relative domain and is mapped upward by the existing window walk. |
| A `cut:Gain` envelope reaches the mix **through the track**, not through the cut | The curve lives on the WINDOW, which may not know its track. `STrack::refreshClipGainCurves()` re-reads every child's lane from `bumpRenderChainEpoch[Range]()` — the one main-thread funnel every model change already passes through, exactly as `refreshInstrumentFeed()` does. A take stack is asked for its ACTIVE take, so an inactive take keeps its own envelope. |
| A slot's lane needs its TRACK to do the invalidation walk | `SObject::invalidateRenderPathRange()` from an `SPluginSlot` is a NO-OP: the walk goes down from the project root through `childLinks()`, and an `SPluginChain` is deliberately not an `SLink` child of its track. The slot emits `audioInvalidatedRange`; `STrack` walks. Same pitfall proposal 08 M5 found for a bypass. |
| `set-track-volume` / `set-track-mute` on a **Read-family** lane write a POINT at the locator | Otherwise the fader moves, the render does not, and undo carries a step nobody can hear. Trim and Off are deliberately not redirected — there the static value is still the thing being edited. |
| **Never put an access specifier inside a `slots:` block** | Adding the automation methods inside `strack.h`'s `public slots:` demoted `trackEventClipChanged` to a plain member. It is connected by NAME, so the connect failed at RUNTIME and every event-clip edit silently stopped reaching the render — invisible at compile time, and it cost a bisect back to the base commit. |

Gates: the qxa cases `automation_volume_ramp`, `automation_mute_step`,
`automation_plugin_param`, `automation_clip_gain`, `automation_edit_invalidates`,
`automation_stereo` plus `action_roundtrip_test`. The fixture is `tests/test_autosaw.wav`
(`tests/tools/gen_auto_fixture.py`): 4 s of a 480 Hz sawtooth whose period is
EXACTLY 100 frames, which is what makes every per-second, per-1000-frame and
~2 ms window in those cases a closed form rather than a measurement.
**STEREO IS GATED SINCE 2026-08-17** by `automation_stereo` — those five cases
read CHANNEL 0 only, because P5 predates the wide sink (36-B5), and nothing in
them could tell a per-channel gain from a gain applied to channel 0 and copied
outward. The new case asserts the ramp per second on channel 0 AND channel 1 of
`test_stereo.wav`'s 6 dB ladder (closed form, scaled by the ladder), that the
image survives the ramp, and that a `param:` step on `tw.test.clap.stereoskew`
moves both channels while keeping the skew's ratio of 4. NOT gated: pan
(`self:Pan` is still unimplemented — the wide sink removed the reason it could
not be heard, not the work; proposal 37 §12 leaves it to a later proposal),
placement-scope envelopes (32), and `cut:VelocityScale` / `cut:Transpose`, which
are implemented and round-trip but have no dedicated case. The mode UI and Touch/Latch/Write RECORDING landed in P6 — see below.

### The automation UI (P6 — executed 2026-08-16)

Automation lanes are on screen and editable. Invariants:
`main/timeline/CONTRACT.md` inv. 17-20, `main/shell/CONTRACT.md` inv. 11,
`main/pluginui/CONTRACT.md` inv. 9, `main/testkit/CONTRACT.md` ("Automation UI
gestures").

| Thing to know | Why |
|---|---|
| An automation lane is an `STrackRow` **SUB-LANE** (`subKind {None, Take, Automation}`), and the whole feature is ONE new file, `timeline/src/sautomationlane.{h,cpp}` | Being a sub-lane buys inv. 5's geometry, the track's single head spanning the group, and `assert-lane-alignment` for free. `sstdmixerview.cpp` is already the largest file in the app, so even the five `SStdMixerView` members that are automation code are DEFINED in the new file — the arranger keeps only the call sites (+36 lines for the feature). |
| The curve is sampled **per pixel through `SAutomationLane::valueAt`** | The same call `assert-automation-value` makes. Step / Linear / Exp come out right by construction; a per-segment painter would be a second implementation of the interpolation and could disagree with the ear. |
| Each target draws its **own** domain (`sAutoScaleFor`) | dB through THE fader curve for `self:Volume` (so a given dB sits at the same fraction of the lane as of the fader), 0/1 stepped for `self:Muted`, the plugin's DECLARED range for `param:<id>` (via the new `SPluginSlot::paramRows()`, because timeline may not include `tw/plugins`), a linear factor over [0,1] for `cut:Gain`. A shared 0..1 scale draws a −60 dB fade as a flat line on the floor. |
| Gestures **revert, then act** | The live drag mutates the point table for feedback and pushes nothing; the release puts the pre-drag table back BEFORE submitting the verb. Skip the revert and the action finds nothing to change: its undo step is a no-op and a redo double-applies. |
| ONE pruning walk for EVERY per-track UI-state set (`pruneUiState`) | Fold set, take-lane set, height scales, shown-automation set — all keyed by `STrack*`. There was NO pruning before P6, so a removed track left a dangling key for a later track at the same address to inherit. The walk is over the MODEL: a collapsed folder's children are alive and have no row. |
| The head **"A" button governs EVERY lane the track owns** — its own and its slots' — in one undo macro | The only reading under which a single button is not ambiguous the moment a track owns two lanes. The button keeps the letter A at every density (three of the six modes start with a letter another 20 px square already uses); the mode is colour + tooltip on screen and `Amode=` in `describeHead()`, appended AFTER `name=` so every P4 `contains=` string still matches. |
| **Touch/Latch/Write are UI recorders and commit ONE `set-automation-points` per gesture** | `SAutomationRecorder` in `main/shell` — both the arranger's fader (`app/timeline`) and the plugin parameter slider (`app/pluginui`) feed the same pass, and those two modules cannot see each other. Bounded by the TRANSPORT: `setPlaying(false)` commits. Touch releases at the control, Latch holds to the stop as ONE extra point, Write additionally opens its window where the RUN started. An action per tick would put thirty entries a second on the undo stack. |
| A control write during a pass must NOT submit its ordinary verb | `applyVolume_` / `onParamSliderChanged` offer the value to the recorder first and return if it was taken. |
| The fader and the parameter slider **display the READ value** while a Read-family lane exists | Pumped from `SApplication::meterTick` — the one main-thread tick that keeps running at a static position and for a tail after the transport stops (proposal 34). A control being RECORDED is exempt: it shows the hand, not the curve. |
| The `cut:Gain` envelope is an **overlay on the clip**, drawn by the cut renderer after `drawWarpMarkers`, and its gestures are ARMED (off by default) | The curve lives on the WINDOW and travels with it across placements and takes, so a lane on the track would be lying about what it belongs to. Off by default is what keeps every clip-body gesture — move, slip, duplicate, stretch — exactly as it was. |
| **Plugin-gesture punch-in is NOT wired** | `ParamGestureBegin/End` do come out of the CLAP and VST3 backends, but only into `twEventOut` inside `process()` — a worker thread, at freeze time — and nothing consumes that stream; there is no native plugin editor either (proposal 33 M3). The app's own slider press/release is the punch-in. |

**A P5 bug this exposed, now fixed:** `SAutomationLane::setPoints()` used a
non-stable `std::sort` plus `std::unique` (which keeps the FIRST of an equal
run), so `add-automation-point` on a frame that already had a point silently
DROPPED the new value — while its own comment and `docs/ACTIONS.md` both promise
a REPLACE. `stable_sort` + a fold that overwrites. A click that lands a point on
top of another is the commonest automation gesture there is.

Gates: the qxa cases `automation_lane_gestures` (every gesture through the REAL
mouse handlers, one undo step each, head/lane identity with two automation lanes
plus a take lane, a canvas PNG), `automation_write_pass` (a Touch pass over a
real transport, three ticks reverted by ONE undo, and the curve HEARD through
the capture backend — measured to five significant digits of the closed form on
the first run) and `automation_head_mode` (the mode at three densities, plus a
head PNG), plus `action_roundtrip_test`. NOT gated: plugin-gesture punch-in,
Delete over a marquee (a QAction shortcut, not synthesisable from a script),
Latch/Write passes, the read-value display, pixel exactness.

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
`smaragd/tw303a/plugins/CONTRACT.md` (36 of them) and
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
| Scanner + cache | `plugins/src/twpluginregistry.cc`, `twpluginsearchpaths.cc`, `twpluginscancache.cc` | Search paths, `plugincache.v<n>.json`, mtime/size/version keying, sticky `failed`/`timeout` records, cancellable background scan. |
| Crash isolation | `plugins/tools/plugin_probe.cc` → `smaragd_pluginprobe` | One module per child process, driven by `QProcess` with a timeout. A crash becomes a cache record, not a dead app. |
| Slot DSP | `plugins/include/tw/plugins/twpluginslotproc.h` + `src/twplugininsert.cc` | **One wide `twPluginInsert` + one processor** (the plugin's lifetime/state holder). See below; the pre-B4 per-bus tap split is described there too. |
| Placeholder | `plugins/src/twnullplugin.cc` (`createNullPlugin`) | Inert pass-through with the *declared* I/O of a missing plugin, so the graph shape is already the one the real plugin will get. |
| Model | `main/objects/track/spluginchain.cpp`, `spluginslot.cpp` | `SPluginChain` (ordered container) + `SPluginSlot` (descriptor, state chunk, bypass, `reloadPlugin()`). |
| Actions | `main/objects/track/s{insert,remove,reorder}plugin*.cpp`, `ssetplugin{bypass,param}action.cpp` | `insert-plugin`, `remove-plugin`, `reorder-plugin`, `set-plugin-bypass`, `set-plugin-param` — see `docs/ACTIONS.md`. |
| UI | `main/pluginui/` | Browser dialog, FX strip (mounted from `main/timeline/src/strackdetailpanel.cpp`), generic parameter editor. |
| Options | `main/servicesui/src/soptionsdialog.cpp` | The Plugins page: directory list, Rescan now, scan status. |

### One slot, one component, N channels (proposal 36 B4 — the tap split retired)

A slot is **one `twPluginInsert`**: one port in, one port out, N *channels*, and
one `process()` sweep per chunk over every channel. `STrack` builds **one**
`twTrackMix` and **one** `twPluginChain` of that width — not one of each per
bus. Channel-mismatch mapping is derived once from the plugin's *own* reported
layout, against the **page width**: `N→N` Direct, `1→1` on N channels DualMono
(N instances — hence a *factory*, not one instance), `2→2` on one channel
MonoFold (feed both inputs, average the outputs), anything else `Unsupported`
and transparent. That is proposal 08's table unchanged; only where the number
comes from moved.

**What this replaced, and why it existed**, because the shape is still visible
in the git history and in `plan/proposed/08_PLUGIN_HOSTING.md`. Until proposal
36 the frozen page was one *mono* page per component, so N channels had to be N
parallel component *instances*, and a stereo-linked plugin — which must see all
its channels in one `process()` call — could not be expressed by any single one
of them ("a component that wrote interleaved stereo into one page produced
garbage the engine then read as mono"). The slot was therefore split into N
per-bus **taps** around one out-of-band `twPluginSlotProcessor` with a private
all-bus page cache: the first tap to ask rendered every bus by reaching
*sideways* through its siblings, and the rest hit the cache. That sideways
gather is what `plugins/CONTRACT.md` invariant 13's deadlock rule was about, and
it is gone.

**`twPluginSlotProcessor` remains, deliberately, and is no longer graph
machinery.** It owns the plugin **lifetime and state**: the instance(s), the
`prepare()`/activate bookkeeping, the mismatch mapping, the block chunking (4096
frames out of a 65536-frame page) and the position-continuity reset. Proposal 08
invariant 18 depends on exactly that — a slot's identity in the graph *is* its
processor, so a rescan that finds a missing plugin hands the same processor a new
factory rather than re-wiring every chain from the UI. Folding it into the
insert would put a plugin's lifetime inside a component's.

**One cache now sits in front of a plugin edit**, and the components downstream
above it. A parameter write must be followed by
`SPluginSlot::notifyPluginEdited()`, and a bypass must go through
`SPluginSlot::setBypass()`; both emit `SPluginSlot::audioInvalidated()`, which
the owning `STrack` turns into `invalidateRenderPath()`. The slot cannot do that
last step itself — an `SPluginChain` is deliberately *not* an `SLink` child of
its track, so `SObject::invalidateRenderPath()`'s root-down walk never reaches a
slot. Skip either and the edit is completely inaudible. (Before B4 there were
*two* caches here, the processor's all-bus one and each tap's frozen pages; the
first retired with the tap fan-out, because with one insert there are no
siblings to dedup for and the component page cache above is the only cache
there is.)

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
| `<configDir>/plugincache.v<kScannerVersion>.json` | The scan cache (next to `smaragd.ini`), named by `twPluginRegistry::cacheFileName()`. One record per module: path, size, mtime, scanner version, `ok`/`failed`/`timeout`, and the descriptors found. Delete it, or use Rescan with *force*, to clear sticky failures. **The version is in the FILE NAME**, not only in the records: the config dir is shared by every build this user runs, so one file meant a worktree at version N and a worktree at version N+1 rejected each other's records on every launch — a permanently cold cache, a full re-probe of every installed plugin in every process, and (before the teardown fix) a `--test-case` run that passed and then hung until CTest killed it at 600 s. Bumping `kScannerVersion` therefore also renames the file, and every user re-scans once. |
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

`plugins/tests/twtestclap.c` is a real CLAP module built from this repo as
`twtestclap.clap` and copied next to the binary. Four entry points:
`tw.test.clap.gain` (`out = in * gain`, plus a "report block size" mode that
writes the frame count it actually saw, plus — since proposal 37 P2 — a
`Clip Threshold` at **param id 2** that hard-clips AFTER the gain, which is the
order-sensitive fixture the fader-move case needs); `tw.test.clap.stereoskew`
(`out[0] = in[0]*0.5*gain + in[1]*gain`, `out[c>=1] = in[c]*0.5*gain`) — the
cross-channel term is what makes a silent second input visible in a mono render;
`tw.test.clap.sine`, the reference INSTRUMENT (0 audio in, stereo main + mono aux
out, a CLAP|MIDI note port preferring CLAP, 16 envelope-less voices so the RMS of
a held note is `velocity/√2` in closed form and the silence either side is
EXACT, plus — since 2026-08-17 — a **`Stereo Skew` at param id 3**, stepped and
**default OFF**: off, every main channel carries the same sample, which is why no
golden and no pre-existing assertion moved; on, channels 1.. of the main bus are
at HALF amplitude, which is the closed form that makes an instrument's channel
relation assertable from a FILE at all — identical outputs cannot distinguish a
wide sink from one duplicating channel 0); and `tw.test.clap.arp` (note in/out on a fixed 4096-frame grid, so its
output count has a closed form).

`plugins/tests/twtestvst3.cpp` is the VST3 counterpart, built as
`twtestvst3.vst3`. It is C++ because VST3's ABI *is* a C++ vtable, and it links
its own copies of the SDK sources (a module and its host are separate binaries).
`TW Test VST3 Gain` is a 2-in/2-out single component with one `Gain` parameter,
unity by default, which **deliberately ignores `setParamNormalized`**, so a host
that writes the controller and stops there fails the level assertion — the most
common VST3 host bug, made into a regression test. `TW Test VST3 Sine`
(proposal 37 P2) is a SPLIT component/controller instrument: it closes the
"split pair untested" debt, maps CC 7 to Gain through `IMidiMapping` (the only
route a CC has in VST3), honours `sampleOffset`, and **ignores an unactivated
kEvent bus** — so a host that forgets `activateBus` renders silence rather than
failing nothing.

**Events (proposal 37 P2).** The ABI carries notes, CCs, note expressions and
sample-accurate parameter points: `tw/plugins/twpluginevents.h` (`twEventList` /
`twEventOut` / `twProcessContext`, all quoting the ONE `twEvent` from
`tw/events`), plus `capabilities()`, `audioOutBusCount()/audioOutBus(i)` and
`tailFrames()` on `twPlugin`. The new `process()` overload's default forwards to
the legacy one and every backend's legacy overload forwards back with an empty
list — the pre-36 path is the same instructions, which is why no golden moved.
`twNativeInstrument` (`format="tw"`, uid `tw.native.303`) is an in-repo 303
registered like `twPassThrough`, so an instrument is present in every build.
**Nothing above the ABI consumes any of it yet** — the processor/tap split is
untouched, and hosting an instrument is proposal 37 P3b.

Gates: `ctest -R "plugins_test|plugins_scan_test"` and the qxa cases
`plugin_stereo_chain`, `plugin_remove_and_undo`, `plugin_slot_roundtrip`,
`plugin_missing_placeholder`, `plugin_bypass_and_param`,
`plugin_remove_restores_param`, `plugin_ui_strip_and_editor`,
`render_sawtooth_with_effects`.

**The mono-sink gap is CLOSED (proposal 36 B5).** It was recorded here for
five milestones: the graph carried N channels but `RenderSession` and
`AudioEngine` collapsed to one page and duplicated it, so a rendered WAV's two
channels were equal *by construction* and `L != R` was forbidden as a file
assertion. Both stages are now N-channel — `RenderSession` interleaves from one
wide root page into a file of `RenderParams::channels` (the project's), and
`AudioEngine::pullBlock` fills N planar buffers that `twSpeaker` maps onto the
device. `L != R` on a FILE is now a legitimate assertion and
`qxa.plugin_stereo_chain` makes it, bounding channel 1 at 0.5x against channel
0's 1.5x — the skew fixture's cross-channel term reaching a file at last.

**MONITORING IS STEREO, rendering is not.** `twSpeaker` owns the channel
mismatch at the device boundary — the same seam it already owned the
sample-rate mismatch at — and the rule is one line:

```
L = ch0;   R = (projectWidth >= 2) ? ch1 : ch0
```

after which that pair meets the device's own channel count as it always has. A
mono project is standard mono-to-stereo; a stereo project is itself; a project
**wider than two is monitored on its first two channels**, and the rest are
computed in full and **dropped at the device**, deliberately — a fold-down needs
channel roles and a fold law, which proposal 36 §8 names as a non-goal. The
callback logs the reduction **once per width** (never per callback), so somebody
hearing four of their six channels missing can find out why.

**This is the device path only.** A render is `RenderSession`, which shares no
code with it and writes the project's full width: a 6-channel project renders a
6-channel file (`qxa.mc_six_channel`) *and* is monitored in stereo. The rule
lives as a pure function (`twmonitor::pullChannels` / `twmonitor::interleave`)
so `playback_test` can assert it at widths 1/2/6 against 2- and 6-channel
devices without opening one.

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
