# Plan Execution State

Running log of which proposals in `plan/proposed/` have been executed, and the
state they left the repository in. Append a new section each time a proposal is
worked.

---

## 01_BUILD_SYSTEM_MODERNIZATION.md

- **Date:** 2026-05-30
- **Status:** Phase 1 complete (CMake infrastructure). Phases 2–5 pending.
- **Verified on platform:** Windows 11 — `cmake -G Ninja` configure step
  succeeds end-to-end against Qt 6.11.1 (`C:\Qt\6.11.1\mingw_64`) using the
  bundled MinGW 13.1 + Ninja 1.12 toolchain. Build fails at compile time, as
  expected, because of Phase 2 portability work (see below). Linux/macOS
  configure not yet exercised.

### What landed

| File                                        | Purpose                                                                                  |
|---------------------------------------------|------------------------------------------------------------------------------------------|
| `smaragd/CMakeLists.txt`                    | Top-level project: C++17, Qt5 find_package, platform detection, audio-backend options.   |
| `smaragd/tw303a/CMakeLists.txt`             | Static `tw303a` library; per-platform backend wiring (ALSA, WASAPI, CoreAudio, etc.).    |
| `smaragd/main/CMakeLists.txt`               | `smaragd` executable; platform-appropriate target type (WIN32 / MACOSX_BUNDLE / ELF).    |
| `docs/BUILD.md`                             | Per-platform build instructions.                                                         |
| `.gitignore`                                | Added CMake / qmake build-output and IDE patterns.                                       |

### What was deliberately deferred

- **Phase 2 — audio abstraction layer.** `tw303a/src/twspeaker.cc` still
  unconditionally includes Linux POSIX headers (`unistd.h`, `sys/ioctl.h`,
  `syslog.h`) and contains the deprecated pre-10.5 CoreAudio code path. The
  CMake will configure on macOS/Windows but compilation will fail there until
  the `AudioBackend` interface and concrete backends land. ALSA Linux builds
  should continue to work unchanged.
- **Phase 3 / 4 — build variants & dep management.** The CMake supports
  multi-backend selection via `-DENABLE_*` flags and uses `find_package` /
  `pkg_check_modules` for native deps; no Conan/vcpkg integration was added.
- **Phase 5 — IDE integration.** No `.vscode/`, no Xcode/VS solution
  generators wired into helper scripts. The instructions in `docs/BUILD.md`
  cover the standard `cmake -G` invocations.
- **CI.** No GitHub Actions workflows were added (deliverable was listed as
  DevOps-owned in the original plan).
- **qmake removal.** Both build systems coexist as the original migration path
  specified. `smaragd.pro` and `smaragd/build` remain.

### Verification status

**Windows / Qt6 / MinGW — configure:** ✅ Succeeds.

```
cmake -S smaragd -B smaragd/build -G Ninja `
      -DCMAKE_PREFIX_PATH="C:/Qt/6.11.1/mingw_64"
```

CMake auto-detects Qt 6.11.1, sets `QT_VERSION_MAJOR=6`, picks the WASAPI
backend (placeholder), and writes a Ninja build. AUTOMOC, AUTORCC, AUTOUIC
all initialise cleanly. No warnings about missing modules.

**Windows / Qt6 / MinGW — build:** ❌ Fails at compile time. Expected. The
following files in `tw303a/src/` `#include` Linux-only POSIX headers
unconditionally:

| File                  | Offending include              |
|-----------------------|--------------------------------|
| `twmixer.cc`          | `<syslog.h>`                   |
| `twlatch.cc`          | `<syslog.h>`                   |
| `twpipe.cc`           | `<syslog.h>`                   |
| `twsimplesaw.cc`      | `<syslog.h>`                   |
| `twspeaker.cc`        | `<sys/ioctl.h>`, `<unistd.h>`, `<syslog.h>` |
| `twconstant.cc`       | `<syslog.h>`                   |
| `twcomponent.cc`      | `<syslog.h>`                   |
| (likely more)         | (build stops at first failures)|

These are the Phase 2 work-items; the CMake itself is fine.

**Sanity checks (manual):**

- All 21 `.cc` sources in `tw303a/src/` (including `twlatch.cc` and
  `twstreaminglatch.cc`, which live in src but are not declared in the
  `HEADERS` of the .pro file) are listed.
- All 22 `.cpp` sources in `main/src/` are listed.
- All headers with `Q_OBJECT` are included in target source lists so AUTOMOC
  will pick them up (verified: 7 in `tw303a/include`, 19 in `main/include`).
- Include directories match the qmake `INCLUDEPATH`:
  `include`, `main/include`, `tw303a/include`.
- ALSA compile definition (`QBX_LINUX_ALSA=1`) and link (`asound`) match the
  qmake `.pro`.

**Linux / macOS:** Configure not exercised. Should work given the same CMake
runs on the more restrictive Windows path, but should be confirmed.

### Next actions

1. **Linux smoke build.** Confirm `cd smaragd && cmake -B build &&
   cmake --build build` produces a working binary on a Linux box — the
   existing source is Linux-shaped, so this should work end-to-end and is a
   prerequisite for trusting the CMake migration.
2. **Phase 2 (audio abstraction).** Replace the platform `#ifdef` sprawl in
   `tw303a/src/twspeaker.cc` (and the `<syslog.h>` calls scattered through
   the engine) with a portable logging shim plus an `AudioBackend`
   interface. Backend `.cc` files drop into `tw303a/src/audio/` and wire
   into the existing `if(ENABLE_*)` blocks in
   `smaragd/tw303a/CMakeLists.txt`. See `plan/proposed/02_AUDIO_DRIVER_STRATEGY.md`.
3. Once Phase 2 lands, re-run the Windows build (CMake configure is already
   green) and add macOS.
4. Add a CI workflow (`.github/workflows/build.yml`) once at least Linux is
   green end-to-end.

### Risks / unknowns

- **MOC paths.** Several `Q_OBJECT` headers (e.g. `tw303a/include/twspeaker.h`)
  pull in optional platform headers via `#ifdef`. AUTOMOC parses headers
  without those defines visible; if a `Q_OBJECT` class's signal/slot signature
  depends on a platform-only type, AUTOMOC may fail to generate a working
  moc file. Hasn't been observed, but worth watching during first build.
- **`twlatch.cc` / `twstreaminglatch.cc`.** Present on disk but missing from
  `smaragd.pro`'s `SOURCES`. Included here on the assumption they are needed —
  the qmake build may have been omitting them by mistake, or they may be
  unused. Confirm during first Linux build.
- **Qt 5 EOL.** Qt 5.15 LTS reaches end of free support in 2026. A follow-up
  proposal for Qt 6 migration may be worthwhile.

---

## 01_BUILD_SYSTEM_MODERNIZATION.md — Phase 2 (audio abstraction)

- **Date:** 2026-05-30
- **Status:** Phase 2 of the build-system proposal complete — the synthesizer
  engine (`tw303a`) now compiles on Windows. The deeper audio work in
  `plan/proposed/02_AUDIO_DRIVER_STRATEGY.md` (concrete WASAPI/CoreAudio
  backends, PipeWire/JACK, device enumeration UI, sample-rate negotiation)
  remains future work.
- **Verified on platform:** Windows 11 — `cmake --build build --target tw303a`
  produces `build/lib/libtw303a.a` (~8.8 MB) with Qt 6.11.1 + MinGW 13.1.
  `smaragd` executable does NOT yet link on Windows; the remaining failures
  are Qt6 source-porting issues in `main/` (`qxml.h` no longer in Qt6,
  `(unsigned long)ptr` truncation on LLP64), not audio.

### What landed

| File                                                 | Purpose                                                                              |
|------------------------------------------------------|--------------------------------------------------------------------------------------|
| `smaragd/include/twsyslog.h`                         | Portable `syslog()` / `LOG_*` shim — POSIX passes through, Windows routes to stderr. |
| `smaragd/tw303a/include/audio/audio_backend.h`       | `audio::AudioBackend` interface + `AudioConfig` + `createAudioBackend()` factory.    |
| `smaragd/tw303a/include/audio/null_backend.h` + `.cc`| No-op backend used when no concrete backend is enabled. Lets the app link.           |
| `smaragd/tw303a/include/audio/alsa_backend.h` + `.cc`| ALSA backend extracted from `twspeaker.cc`. Behaviour-preserving (44.1k/S16_LE/stereo, 1024-frame buffer, 64-frame period, async callback). Adds xrun recovery the original lacked. |
| `smaragd/tw303a/src/audio/audio_backend.cc`          | Factory: returns `ALSABackend` when `QBX_LINUX_ALSA` is defined, else `NullBackend`. |
| `smaragd/tw303a/src/twspeaker.{h,cc}` (rewritten)    | Holds `std::unique_ptr<AudioBackend>`. `startOutput()` installs a render callback that pulls from `pInputPlugs[0]` and fans mono → N channels in place. All platform `#ifdef`s removed. Deleted: the broken pre-2005 `QBX_MAC_OSX_10_2` block, the unused `QBX_LINUX_OSS` socket-notifier path, the `unistd.h`/`fcntl.h`/`sys/ioctl.h`/`linux/soundcard.h` includes. |
| 12 `tw303a/src/*.cc` files                            | `#include <syslog.h>` → `#include "twsyslog.h"`. No other changes.                   |
| `smaragd/CMakeLists.txt`                             | `ENABLE_WASAPI`/`ENABLE_COREAUDIO` default flipped to `OFF` since their backends do not exist yet — `NullBackend` is what links on Windows/macOS until they're written. |
| `smaragd/tw303a/CMakeLists.txt`                      | Wires `audio_backend.cc`/`null_backend.cc` into the always-on source list. ALSA's `alsa_backend.{h,cc}` added under the existing `if(SMARAGD_LINUX AND ENABLE_ALSA)` block. |

### What was deliberately deferred

- **WASAPI backend** — needs ~400 lines of COM-heavy code (IMMDeviceEnumerator,
  IAudioClient, IAudioRenderClient, event-driven render thread). The link
  flags and `#define QBX_WIN_WASAPI` are already wired in the CMake; flipping
  the option ON and dropping `wasapi_backend.{h,cc}` next to the ALSA backend
  is all that's needed.
- **Modern CoreAudio backend** — same shape; replaces the deleted pre-2005
  `OpenAComponent` / `FindNextComponent` code with `AudioComponentInstanceNew`
  / `AudioComponentFindNext` against an `AUGraph` or raw `AudioUnit`.
- **PipeWire / JACK / PulseAudio** — `pkg_check_modules` already wired; backend
  files (`pipewire_backend.cc` etc.) and the factory's `#if defined(QBX_…)`
  branch are the only missing pieces.
- **ALSA modernization** — device enumeration via `snd_card_next` /
  `snd_ctl_*`, dynamic sample-rate/format negotiation, configurable latency.
  Current backend keeps the original hard-coded settings.
- **Device-selection UI** — outside the audio-engine layer entirely.

### Verification status

- **Windows / Qt6 / MinGW — tw303a:** ✅ Library builds cleanly.
  `cmake --build build --target tw303a` → `build/lib/libtw303a.a` (8.8 MB).
- **Windows / Qt6 / MinGW — smaragd executable:** ❌ Still fails, but for
  reasons unrelated to Phase 2:
    1. `main/include/sprojectloader.h` includes `<qxml.h>`. Qt6 dropped
       `QtXml`'s lowercase compat header; the include needs to become
       `<QXmlStreamReader>` (and the SAX-style XML reader needs porting to
       streaming).
    2. `main/src/{slink,sobject,sproject}.cpp` cast pointers to `unsigned
       long` for serialization. On 64-bit Windows (LLP64) `unsigned long` is
       32 bits, so the cast loses precision. Should be `uintptr_t`.
  These are LLP64 + Qt6-source-port concerns that belong in a separate
  proposal. They are pre-existing issues, not introduced by this work.
- **Linux:** Not exercised. ALSA backend is behaviour-preserving relative to
  the original `twspeaker.cc` ALSA path; a Linux developer should still
  smoke-test before trusting it.

### Behaviour-relevant changes (heads up for a Linux smoke test)

- xrun recovery added in `ALSABackend::asyncCallback_` and `writeChunk_`
  (the original code logged but never called `snd_pcm_prepare` on `-EPIPE`).
  This should reduce silent audio dropouts under load.
- Mono → stereo fan-out now happens in `twSpeaker`'s render callback, not
  inside the ALSA write loop. Behaviour matches the original (same int16
  sample value duplicated to both channels) but the conversion runs against
  floats first and only the ALSA backend converts to S16.
- `setGlobalLocatorPos` is now called from inside the audio callback. In the
  original this was also true (it ran inside `fillBuffer()` from the async
  handler), so threading semantics are unchanged.

### Next actions

1. Linux smoke test of the refactored ALSA path.
2. Separate proposal: Qt6 source porting + LLP64 pointer fixes for `main/`
   (`qxml.h`, `(unsigned long)ptr` casts, `qsocketnotifier.h` lowercase
   compat headers in surviving files). Needed before any Windows build of the
   full executable.
3. Resume `02_AUDIO_DRIVER_STRATEGY.md` proper, starting with the WASAPI
   backend now that the abstraction layer exists.

---

## Qt6 source porting for `main/`

- **Date:** 2026-05-30
- **Status:** ✅ Complete. `smaragd.exe` (20.32 MB) builds and launches on
  Windows 11 with Qt 6.11.1 + MinGW 13.1. End-to-end Phase 1 + Phase 2 +
  Qt6-port verification on Windows.
- **Verified:** `cmake --build build` produces `build/bin/smaragd.exe`;
  `Start-Process smaragd.exe` shows the process stays alive (window-up
  smoke test only — no audio/UI interaction tested, NullBackend is active
  on Windows so there is no sound anyway).
- **Not yet covered:** No proposal file exists for this work — it was
  inline cleanup needed to unblock the Windows executable build. If a
  formal Qt6 migration proposal is later wanted (e.g., for a full Qt5→Qt6
  pass across the engine too), this section is the prior art.

### Build-breaking changes

| File                                              | Change                                                                                  |
|---------------------------------------------------|-----------------------------------------------------------------------------------------|
| `main/include/sprojectloader.h`                   | Replaced `#include <qxml.h> / <qdom.h> / <qhash.h>` with `<QDomDocument> / <QHash>`. The SAX API (`<qxml.h>`) is removed in Qt6; the project never used any SAX classes — only `QDom*` — so deletion was safe. |
| `main/src/slink.cpp`                              | `(unsigned long)(&object_)` → `reinterpret_cast<std::uintptr_t>(&object_)`. Fixes LLP64 pointer truncation on 64-bit Windows. |
| `main/src/sobject.cpp`                            | Same fix at line 61.                                                                    |
| `main/src/sproject.cpp`                           | Same fix at line 32.                                                                    |
| `main/src/sprojectloader.cpp`                     | `QString::null` (removed in Qt6) → `id.isNull()`.                                       |
| `main/src/smainwindow.cpp`, `scut.cpp`, `sprojectloader.cpp`, `sstdmixer.cpp`, `strack.cpp` | `qWarning() << ... << endl` → `... << Qt::endl`. In Qt6 the std::endl manipulator is no longer accepted by QDebug — must use `Qt::endl`. (Remaining `<< endl` occurrences in `tw303a/` are inside `#ifdef`-disabled or commented-out blocks; harmless.) |
| `main/src/main.cpp`                               | `int main(int argc, char *const argv[])` → `int main(int argc, char *argv[])`. Qt6 on Windows uses an entrypoint shim (`Qt6::EntryPoint`) that `#define`s `main` → `qMain` and expects the exact `int(int, char**)` signature — the `char *const` variant produced an `undefined reference to qMain(int, char**)` link error. |

### CMake changes

- `main/CMakeLists.txt` adds `${CMAKE_SOURCE_DIR}` to the smaragd target's
  include directories so `#include "pix/playoff.xpm"` resolves (the XPM
  icons live under `smaragd/pix/`, not `smaragd/main/pix/`).
- `main/CMakeLists.txt` drops `include/ssortedobjlist.h` from the headers
  list — the file is empty (1 line, no content), and AUTOMOC was warning
  about it on every build. Pre-existing issue inherited from the .pro.

### Deferred (deprecation warnings, not errors)

The build is green but produces ~15 deprecation warnings:

- `QMessageBox::information(... int, int)` — use the `StandardButtons` overload.
- `QMenu::addAction(text, receiver, member, shortcut)` — argument order
  changed; use the modern signature.
- `Qt::operator+` on `Qt::Modifier | Qt::Key` — replace `+` with `|`.
- `XPM string-to-char*` warnings — vendor of XPM-format includes.

None are blockers. They belong in a "polish" pass alongside the broader
Qt5→Qt6 idiom cleanup (e.g. lowercase `<qfoo.h>` includes throughout).

### Next actions

1. Linux smoke test of the refactored ALSA path remains the most valuable
   thing to do next (still untouched since Phase 2 was authored).
2. macOS configure — should now work too, but compile will fail until a
   modern CoreAudio backend exists (the NullBackend takes over silently).
3. Resume `02_AUDIO_DRIVER_STRATEGY.md` Phase 4 (WASAPI implementation).

---

## 02_AUDIO_DRIVER_STRATEGY.md — WASAPI backend (Windows)

- **Date:** 2026-05-30
- **Status:** First real backend behind the abstraction. Builds clean on
  Windows / Qt6 / MinGW; manual playback verification (clicking Play in
  the UI and listening) is the user's job and has NOT been done from
  this session.

### What landed

| File                                                     | Purpose                                                                                              |
|----------------------------------------------------------|------------------------------------------------------------------------------------------------------|
| `smaragd/tw303a/include/audio/wasapi_backend.h`          | `WASAPIBackend` class; forward-declares COM interfaces so `<windows.h>` does not leak into Qt MOC.   |
| `smaragd/tw303a/src/audio/wasapi_backend.cc`             | Implementation: shared-mode IAudioClient, event-driven render thread, MMCSS "Pro Audio" priority, format conversion for float32 / int16 / int32 device mix formats. |
| `smaragd/tw303a/src/audio/audio_backend.cc`              | Factory: `QBX_WIN_WASAPI` branch picks `WASAPIBackend` over `NullBackend`.                           |
| `smaragd/tw303a/CMakeLists.txt`                          | Wires `wasapi_backend.{h,cc}` under the existing `if(SMARAGD_WINDOWS AND ENABLE_WASAPI)` block.       |
| `smaragd/CMakeLists.txt`                                 | `ENABLE_WASAPI` default flipped back to `ON` on Windows.                                              |

### How it works

- `openDevice()`: `CoInitializeEx` → `IMMDeviceEnumerator` → default render
  endpoint → `IAudioClient::Activate` → `GetMixFormat` → `Initialize` in
  `AUDCLNT_SHAREMODE_SHARED | AUDCLNT_STREAMFLAGS_EVENTCALLBACK` →
  `SetEventHandle` → `GetService(IAudioRenderClient)`.
- `startOutput()`: pre-fills one silent buffer, calls `IAudioClient::Start`,
  spawns a dedicated thread.
- Audio thread: boosts itself to MMCSS "Pro Audio" priority, then waits on
  the buffer-ready event. Each wake-up pulls floats from the render
  callback into a scratch buffer and converts in place into the device's
  native format (float32 / int16 / int32) before releasing back to WASAPI.
- `stopOutput()`: sets a stop flag, signals the event to unblock the
  thread, joins, then calls `IAudioClient::Stop`.

### MinGW quirks worked around

- `KSDATAFORMAT_SUBTYPE_IEEE_FLOAT` / `KSDATAFORMAT_SUBTYPE_PCM` are not
  always provided as linker symbols by MinGW's `<ksmedia.h>`; defined as
  file-local GUIDs.
- Same story for `CLSID_MMDeviceEnumerator` /
  `IID_IMMDeviceEnumerator` / `IID_IAudioClient` /
  `IID_IAudioRenderClient` — defined locally rather than relying on the
  symbols being exported by `mmdevapi`.

### Known limitations (deferred)

- **Sample-rate mismatch.** WASAPI shared mode forces the device's mix
  format, which is almost always 48 kHz on modern Windows. The synth
  produces 44.1 kHz samples; playback at 48 kHz will be ~8.8 % too fast
  (and pitched up by the same ratio). The backend logs a `LOG_WARNING`
  to flag this. Fixes:
    - Make the synth rate-aware (engine work, broad scope), OR
    - Add a resampler at the backend boundary (smaller scope), OR
    - Use `AUDCLNT_SHAREMODE_EXCLUSIVE` at 44.1 kHz (intrusive — kicks
      other apps off the device).
- **Default-endpoint only.** No device enumeration / selection UI yet.
- **No endpoint-change handling.** If the user switches default device
  while playing, behaviour is undefined.
- **No format other than float32 / int16 / int32.** 24-bit-packed PCM
  devices would fail openDevice with a logged error.
- **No exclusive-mode path.**

### Verification

- **Build:** ✅ clean on Windows / Qt6 / MinGW;
  `cmake --build build` → `build/bin/smaragd.exe` (20.57 MB), auto-deploy
  copies Qt DLLs.
- **Launch:** ✅ process stays up; no crash on startup. WASAPI init only
  runs when the user clicks Play, so the launch test does not exercise
  the backend itself.
- **Audible playback:** ❓ not tested from this session — needs a human
  with a speaker.

### Next actions

1. **You: launch the exe, click Play, listen.** If nothing happens,
   `stderr` should show `WASAPIBackend: opened default endpoint, …` lines
   from `syslog()`. If you see those but hear nothing, the synth's
   `pInputPlugs[0]` chain is the next suspect, not the backend.
2. Sample-rate handling — pick one of the three options above when
   pitched-up playback gets annoying.
3. Linux ALSA smoke test still outstanding.
4. CoreAudio backend mirrors this shape: device → AudioUnit → render
   callback → IAudioRenderClient analogue.

---

## End-to-end Windows audio + Qt6 polish (post-WASAPI)

- **Date:** 2026-05-30
- **Status:** ✅ `smaragd.exe` produces audible sound on Windows via WASAPI
  from a project created via `File → New`. Build emits no warnings
  beyond vendor XPM noise and a benign DirectX deploy notice.

### Build & launch ergonomics

| Change                                                              | Why                                                                                                  |
|---------------------------------------------------------------------|------------------------------------------------------------------------------------------------------|
| `AUTO_DEPLOY_QT` CMake option (default ON) — runs `windeployqt` / `macdeployqt` as a POST_BUILD step | After `cmake --build`, the exe sits next to all Qt6 DLLs and plugins; double-clickable, no PATH dance. Opt-out with `-DAUTO_DEPLOY_QT=OFF`. |
| `SMARAGD_WIN_CONSOLE` CMake option (default ON) — omits the `WIN32` linker flag                       | Default Windows build uses the CONSOLE subsystem so stdout/stderr from the launching shell receive logs. Was previously `WIN32` (GUI subsystem) and stderr went into the void. |
| `twsyslog.h` shim: add `fflush(stderr)` after each call                                                | Without it, redirected stderr is fully buffered and logs were lost on abnormal exit.                 |

### Wiring chain (the actual reason audio worked)

The audio-output chain had a stack of latent bugs that all had to be
fixed together to produce sound on a freshly created project:

1. **twRewire was a snapshot patch-bay.** `linkOutput(i)` reached back
   through the rewire and returned a pointer to whatever component was
   currently wired into input `i`, so downstream consumers (the speaker)
   held a pointer to the *upstream* component. Later input swaps left
   the speaker dangling. Rewrote twRewire to own one `twStreamingLatch`
   per output index. `calcOutputTo` now pulls from the matching input
   or `memset(0)`s when nothing's wired. The speaker is connected once
   at project creation and stays valid across graph mutations.
2. **`twStreamingLatch::copyData` hardcoded `0`** as the output index
   when calling its owner component's `calcOutputTo`. Latent bug for any
   multi-output component, harmless for the existing single-output ones
   — but needed for twRewire's per-output latches to each fill from the
   correct input. Now passes `getIndex()`.
3. **SStdMixer's constructor called `setNBusses(0)`**, which shrank the
   rewire to zero outputs, so `linkOutput(0)` was out of bounds and
   returned NULL the instant `SApplication::setCurrentProject` tried to
   wire the speaker. Replaced with `setNBusses(1)` — every project
   starts with one bus that the speaker can permanently attach to.
4. **`setNBusses`'s bus-creation loop indexed `children.at(i)`** without
   a bounds check — dormant when called with `n=0`, asserted as soon
   as the new `setNBusses(1)` default tried to create a bus before any
   tracks existed. Bounded.
5. **`setNBusses` then primed mixer input levels using the bus index**
   (`mix->setInputLevel(i, lk->getVolume())` where `i` is the bus index)
   and duplicated a `volumeChanged` signal connection already done
   per-track in `insertTrack`. Both removed; `reconnectTracksToMixer()`
   is now called at the end of `setNBusses` to do the wiring correctly.

### Cleanup pass — quirks + Qt6 polish

- Pointer-truncating debug logs in `sstdmixer.cpp`, `sobject.cpp`,
  `sstdmixerview.cpp`: `(unsigned)(ptrdiff_t)ptr` → `%p` + `(void *)ptr`.
- `SMVActualView::globalLocatorMoved` was constructing a QPainter
  outside `paintEvent` — Qt6 floods stderr with `QWidget::paintEngine:
  Should no longer be called` on every audio render tick. Replaced
  with `update(x, 0, 1, h)` calls; `paintEvent` already redraws the
  playhead.
- Qt6 deprecations all fixed: `QMessageBox::information(..., "OK")` →
  `..., QMessageBox::Ok`; `QMenu::addAction(text, recv, member, shortcut)`
  → `addAction(text, shortcut, recv, member)`; `Qt::CTRL + Qt::Key_X`
  → `Qt::CTRL | Qt::Key_X`.
- General compiler warnings: `register` keyword, `catch(excStandard e)`
  → `catch(excStandard &e)` + bare `throw`, `strncpy` of fixed-width
  WAV chunk IDs → `memcpy`, four set-but-unused locals removed,
  redundant `NOMINMAX` define in `wasapi_backend.cc` dropped.

### How to build & run today

```powershell
$env:PATH = "C:\Qt\Tools\mingw1310_64\bin;C:\Qt\Tools\Ninja;" + $env:PATH
cd smaragd
cmake -B build -G Ninja -DCMAKE_PREFIX_PATH="C:/Qt/6.11.1/mingw_64"
cmake --build build
& .\build\bin\smaragd.exe
```

In the app: `File → New`, then add a track, import a sample, hit Play.

### Open issues still on the table

- **Sample-rate mismatch.** Synth produces 44.1 kHz, WASAPI shared mode
  forces the device rate (almost always 48 kHz). Playback is ~8.8 %
  pitched up. Three options: rate-aware synth (engine work), resampler
  at backend boundary (medium), exclusive-mode at 44.1 (intrusive).
- **WASAPI: default endpoint only**, no device picker, no
  format-change handling, no exclusive-mode path, only float32/int16/int32.
- **Linux ALSA refactor untested** — behaviour-preserving rewrite +
  added xrun recovery, but needs a Linux smoke build.
- **CoreAudio / PipeWire / JACK / PulseAudio backends** — abstraction
  is ready, no implementations.
- **CI workflow** — `.github/workflows/build.yml` not yet authored.
- **`paintEvent` cursor still uses `CompositionMode_Xor`** — harmless
  but vestigial from the old XOR-draw scheme.
- **`reconnectTracksToMixer`** has a `printf` debug spam ("Calling
  $X->setInput(...)") that's now correctly formatted with `%p` but
  still noisy. Could be downgraded to `qDebug` or removed.

### Next session (planned)

Discuss an architecture for representing user actions in two related
forms: an undo/redo stack and a scripting interface. Both want a
uniform "what just happened, replayably" representation.

---

## 03_ACTION_MODEL.md (proposed)

- **Date:** 2026-05-30
- **Status:** Design only — proposal authored, no code landed yet.

Captures the SAction design discussed in-session: an immutable command
object that serves as the GUI→engine handoff primitive, the undo/redo
unit, and (later) the scripting verb. Engine-thread `apply()` produces
the inverse from observed pre-state; GUI-side `tryCancel` on the queue
gives near-zero-latency undo for actions still in flight. Pending
actions are persisted inline in the project XML so save never blocks
on queue drain — addresses the busy-DAW save/undo pathology that
motivated the work. Selection is treated as first-class project state
(Photoshop pattern) and participates in undo/serialization/scripting.

See `plan/proposed/03_ACTION_MODEL.md` for the full interface sketches,
threading model, save-format shape, and phased rollout plan.

---

## 04_WIRE_FORMAT_AND_SAMPLE_RATE.md (proposed)

- **Date:** 2026-05-31
- **Status:** Design only — proposal authored, no code landed yet.

Tackles the sample-rate mismatch (synth 44.1 kHz vs WASAPI 48 kHz device,
~8.8 % pitch error) by making data format a first-class property of each
wire rather than an implicit engine-wide assumption. Two layers:

- **(a) Technical** — a `twFormat` descriptor (sample rate, binary sample
  type, channels, layout) attached to the producing `twLatch` and queried by
  the consumer via `twLatchOutput::getFormat()`. The sink decides how/if to
  convert: consume natively (zero-copy when matched), convert explicitly via
  shared `twConvert`/`twResampler` utilities, or refuse + log. Default
  `twFormat` is mono float32 — byte-identical to today's universal
  assumption — so no existing component needs edits to keep working.
- **(b) Usability** — a settable, persisted project sample rate seeds
  `tw303aEnvironment` (`setSRate`/`sampleRateChanged`) and every wire's
  default format. A common rate by default (proposal recommends 48000 for
  `File → New`), but per-wire override means foreign-rate signals (imported
  96 kHz samples, rendered stems) are reconciled by the downstream sink, not
  forced.

On top of the per-wire format mechanism sits an explicit **(re)negotiation
protocol**. Because a node's output caps depend on its input caps and vice
versa (passthrough: out==in; mixer: common rate; only a resampler decouples),
resolution is a **fixpoint**, not a single pass. It is made to terminate by
construction: capabilities are narrowed over a *finite discrete candidate set*
`D` = {standard rates 44.1/48/88.2/96/…} ∪ {rates hard-anchored in the graph},
and each node's `narrowCaps` coupling relation is **monotone (remove-only)**, so
AC-3-style propagation settles in ≤ Σ|domains| steps (an iteration cap is only a
bug-backstop). The standard rates do triple duty — finite domain (the real
termination guarantee), convergence magnet/tiebreak, and default. The standard
set defaults to **{44100, 48000, 88200, 96000}** and is **configurable** (lives
on `tw303aEnvironment`, persisted with the project). A graph-level
`twNegotiator` builds `D`, propagates to fixpoint, heals empty wires by inserting
`twResampler` nodes (each wire healed at most once → outer loop also terminates),
resolves residual freedom by preference (project rate → device rate → standard
ranking), then calls each node's `commitFormats()` to do its own (potentially
expensive) setup — resampler kernels, buffer allocation — entirely off the
realtime path. Triggered before play, and re-triggered via signalling
(`renegotiationRequired`/`formatChanged`) when any node's constraints change —
e.g. the audio driver switching the device from 48 kHz to 44.1 kHz, which can
ripple upstream so the synth produces the new rate directly and the speaker
resampler collapses to a passthrough. Any node can originate a renegotiation,
not just the sink. This was the key refinement over the initial "sink decides
at read time" sketch (GStreamer caps-negotiation / CoreAudio AUGraph pre-roll
precedent).

The reported bug is closed by phase 2 alone (a `twResampler` inside
`twSpeaker`'s render callback targeting `backend_->getConfig().sampleRate`),
before the engine core is made rate-aware (phase 5: replacing literal
`44100`s in `twsaw`/`twsimplesaw`/`twpipe`/`twmoog`/`twtestseq`/`twwav` with
`env.getSRate()`) and before full graph negotiation (phases 6–7). Open design
forks: negotiation anchor priority (device vs. fixed-rate source), mid-play
local absorption vs. stop/renegotiate/restart, and failure UX. Cross-references
`03_ACTION_MODEL.md` — changing the project rate should eventually be a
`set-project-rate` SAction.

See `plan/proposed/04_WIRE_FORMAT_AND_SAMPLE_RATE.md` for the full descriptor,
read-path API, conversion strategy, worked examples, and 6-phase rollout.

---

## qmake build system removal

- **Date:** 2026-05-30
- **Status:** ✅ Legacy qmake artifacts deleted. CMake is now the sole
  supported build system, completing step 3 of the migration path laid
  out in `plan/proposed/01_BUILD_SYSTEM_MODERNIZATION.md`.

### What was removed

| Path                         | Role                                                                    |
|------------------------------|-------------------------------------------------------------------------|
| `smaragd/smaragd.pro`        | Auto-generated qmake project (by `mkpro`).                              |
| `smaragd/Makefile`           | qmake-generated GNU Makefile (Qt 5.9.5, Linux toolchain).               |
| `smaragd/build.sh`           | Wrapper that ran `mkpro`, `qmake -o Makefile`, `make`.                  |
| `smaragd/mkpro`              | Shell script that regenerated `smaragd.pro` from `find`-discovered sources. |
| `smaragd/.qmake.stash`       | qmake's per-configuration cache (was tracked, shouldn't have been).     |

### Doc / config touch-ups

- `.gitignore` — dropped the four-line "qmake build output" section
  (`smaragd/bin/`, `smaragd/obj/`, `smaragd/generated/`,
  `smaragd/Makefile`, `smaragd/.qmake.stash`). The CMake `build/`
  ignore already covers all current build output.
- `docs/BUILD.md` — removed the transitional preamble ("legacy qmake
  `.pro` file is still in the tree…") and the entire "Switching back
  to qmake (transitional)" section.
- `docs/PROJECT_OVERVIEW.md` — removed the `smaragd.pro` and `Makefile`
  entries from the directory tree, replaced with `CMakeLists.txt`.

### What was deliberately left alone

- `plan/proposed/01_BUILD_SYSTEM_MODERNIZATION.md` — historical proposal
  document. Step 3 of its "Migration Path" ("Remove qmake once all
  platforms verified") is now what this section records.
- `smaragd/main/CMakeLists.txt`'s reference to the `Qt6::qmake` IMPORTED
  target — this is the standard Qt CMake idiom for locating the Qt bin
  directory (to find `windeployqt`/`macdeployqt`). The `qmake` binary
  itself ships with Qt6 as a build-system-detection helper; the name
  predates CMake's adoption but the import is current.

### Verification

- `git ls-files | Select-String -Pattern 'qmake|\.pro$|mkpro|smaragd/Makefile|build\.sh'`
  returns no matches.
- No code in the repo references the removed files.
- Linux build via qmake is no longer possible from this repo — only
  CMake. (CMake on Linux was the original baseline anyway; the
  in-flight ALSA smoke test still applies.)

---

## 04_WIRE_FORMAT_AND_SAMPLE_RATE.md — phases 1-3 (format mechanism + bug fix)

- **Date:** 2026-05-31
- **Status:** Phases 1-3 of the proposal complete and committed. **The ~8.8 %
  pitch/speed bug is fixed** (phase 2). Phases 4-7 (project rate, rate-aware
  engine core, capabilities, negotiation pass) remain — the user opted for the
  full 7-phase rollout, so they are next.
- **Verified on platform:** Windows 11 — `cmake --build smaragd/build` produces
  `build/bin/smaragd.exe` (~21.7 MB) clean on Qt 6.11.1 + MinGW 13.1 (only the
  pre-existing vendor XPM `-Wwrite-strings` noise). **Audible** verification
  (launch, click Play, listen) is still a human step and has NOT been done from
  this session.

### What landed

| File | Purpose |
|------|---------|
| `tw303a/include/twformat.h` (new) | `twFormat` value type: `sampleRate`, `twSampleType` (Float32/Float64/Int16/Int32), `channels`, `twLayout` (Interleaved/Planar). `bytesPerSample`/`bytesPerFrame`/`sameMemoryShape`/`==`. `twCanonicalFormat(rate)`. Default-constructed == today's mono Float32. |
| `tw303a/include/twcomponent.h` | `twLatch::getFormat()` (virtual, default canonical at env rate); `twLatchOutput::getFormat()` delegates to producing latch; `twLatchStreamingOutput::readRaw(void*, frames)`. |
| `tw303a/src/twlatch.cc` | `twLatch::getFormat()` impl (reads `component.env` via existing friendship); `readRaw` impl (mirrors `readStreamingData` while latches store float). |
| `tw303a/include/twresampler.h` + `src/twresampler.cc` (new) | Linear, block-pull mono SRC. Passthrough when rates equal. `process()` reports INPUT frames consumed separately from output frames produced. |
| `tw303a/include/twspeaker.h` + `src/twspeaker.cc` | Holds a `twResampler`; render callback reconciles input-wire rate → device mix rate (`backend_->getConfig().sampleRate`) before the mono→N fan-out. Locator advances by input frames consumed. |
| `tw303a/include/twconvert.h` + `src/twconvert.cc` (new) | `twConvertFrames`: pure interleaved type/channel conversion, no rate change; memcpy fast-path on matching memory shape. |
| `tw303a/src/audio/wasapi_backend.cc`, `audio/alsa_backend.cc`, `src/twwav.cc` | Hand-rolled float→int clip loops replaced by `twConvertFrames`. Int16 clamp standardized to `[-32768, 32767]` (twWav previously clamped to `-32767`). Stale WASAPI "pitch will be off" warning dropped. |
| `tw303a/CMakeLists.txt` | Registers `twformat.h`, `twconvert.{h,cc}`, `twresampler.{h,cc}`. |

### How the bug fix works

`twSpeaker::startOutput` configures the resampler from the input wire's
`getFormat().sampleRate` (currently the env rate, 44100) to the negotiated
device rate (`AudioConfig.sampleRate`, commonly 48000). The render callback
calls `resampler_.process(...)`, which interpolates 44100→48000 (consuming
~940 input frames per 1024 output frames) and reports the input-frame count so
the global locator advances in synth time, not device time. When the rates
already match it is a passthrough — byte-identical to the prior read path.

### Commits

- `997c442` — phases 1-2 (twFormat + speaker resampler).
- `cf98d7c` — phase 3 (native read path + shared converter).
- (this STATE.md entry lands in a follow-up commit.)

### Behaviour-relevant notes / heads-up

- **Resampler quality is linear** for now (proposal earmarks a polyphase/sinc
  upgrade behind the same interface). It removes the pitch/speed error; it is
  not mastering-grade.
- **Int16 clamp range** changed for the WAV writer (`-32767` → `-32768`); a
  1-LSB difference at full negative scale, inaudible, more correct.
- **ALSA port is uncompiled here** (Linux-only); it mirrors the WASAPI change
  and still needs a Linux smoke build.
- First audio callback may grow the resampler's input history buffer once;
  `reserveHint(bufferFrames)` pre-sizes it to avoid steady-state allocation.

### Next actions

1. **You: launch the exe, click Play, listen** — confirm correct pitch/speed
   on a 48 kHz default device. `stderr` will show
   `twSpeaker: resampling 44100 Hz -> 48000 Hz`.
2. Phase 4 — settable + persisted project sample rate
   (`setSRate`/`sampleRateChanged`, candidate-rate set, project-XML attribute,
   `File → New` default 48000).
3. Phase 5 — replace literal `44100`s in the engine DSP with `env.getSRate()`.
4. Phases 6-7 — capabilities (`getInputCaps`/`getOutputCaps`/`narrowCaps`) and
   the `twNegotiator` fixpoint with renegotiation signalling.

---

## 04_WIRE_FORMAT_AND_SAMPLE_RATE.md — phases 4-7 (rate-aware core + negotiation)

- **Date:** 2026-05-31
- **Status:** ✅ All 7 phases of the proposal implemented and committed. The
  full per-wire-format + sample-rate-negotiation design is in the tree, with one
  deliberately deferred sub-step (live resampler-node insertion — see below).
- **Verified on platform:** Windows 11 — `cmake --build smaragd/build` →
  `build/bin/smaragd.exe` (~23.8 MB) clean on Qt 6.11.1 + MinGW 13.1; window-up
  smoke test passes (process stays alive, no startup crash). **Audible**
  verification (launch, click Play, listen) is still a human step.

### What landed (phases 4-7)

| Phase | File(s) | Change |
|-------|---------|--------|
| 4 | `tw303a/include/tw303aenv.h` + `src/tw303aenv.cc` | `setSRate` (emits `sampleRateChanged`); configurable candidate-rate set `candidateRates()`/`setCandidateRates()` default `{44100,48000,88200,96000}` (emits `candidateRatesChanged`); inheritance from `QObject` made **public** so the signals can be connected. |
| 4 | `main/include/sproject.h` + `src/sproject.cpp` | `SProject` holds `sampleRate_` + `candidateRates_`; serializes them on the project root (QTextStream write, QDom read). Fresh project defaults to **48000**; a file without the attribute loads as **44100** and round-trips. |
| 4 | `main/src/sapplication.cpp` | `setCurrentProject` pushes the project's rate / candidate set into the engine (runs again post-load in `fileOpen`, so the engine lands on the saved rate). |
| 5 | `twsaw.cc`, `twsimplesaw.cc`, `twmoog.cc`, `twtestseq.cc`, `twpipe.cc`, `twwav.cc`, `tw303a.cc` | Hardcoded `44100`/`4410000` replaced with `env.getSRate()` (`4410000` == rate·100 fixed-point period). A matched-rate project now plays with the speaker resampler collapsed to a passthrough. |
| 6 | `tw303a/include/twformat.h`, `twcomponent.{h,cc}` | `twFormatCaps` (per-port candidate domain) + `twPortDomains`; `getInputCaps`/`getOutputCaps` (seed mono Float32, any rate) and the monotone `narrowCaps` coupling relation (default: couple all ports to one common rate via set intersection). |
| 7 | `tw303a/include/twnegotiator.h` + `src/twnegotiator.cc` (new) | `twNegotiator`: subgraph discovery from a sink, build candidate domain `D`, AC-3 monotone fixpoint (node `narrowCaps` + wire equality, bounded-iteration backstop), infeasibility detection+logging, preference resolve (project rate first), `commitFormats` per node. |
| 7 | `twcomponent.{h,cc}` | `commitFormats` (default: record formats, emit `formatChanged` on change) + `renegotiationRequired`/`formatChanged` signals. |
| 7 | `twspeaker.cc` | Runs the negotiator before opening the device — **advisory**: logged, playback proceeds regardless, so negotiation can never regress the working audio path. |

### Design notes / decisions

- **Negotiation is advisory, not gating.** Because the speaker's own resampler
  (phase 2) bridges graph-rate → device-rate unconditionally, the negotiator's
  job today is to resolve+commit the graph to the project rate and validate it.
  Making it non-blocking guarantees the shipped pitch fix can't regress if the
  negotiator has a bug.
- **Graph is always uniform today.** Every source produces at the env rate, so
  the candidate-rate intersection is never empty and no wire is infeasible. The
  negotiator's healing path is therefore dormant.
- **Deferred (documented open fork): live resampler-node insertion.** Healing an
  infeasible wire by splicing a rate-converting node into the live graph is
  *detected and logged* but not performed. Rationale: (a) no current graph
  triggers it (needs a fixed-rate source at a non-project rate), (b) it is the
  exact "automatic vs. manual converter insertion" UX fork the proposal flagged
  for later, and (c) live graph mutation can't be runtime-verified in this
  session. To finish it later: add a `twRateConvert` node (1-in/1-out wrapping
  `twResampler`, `narrowCaps` returns false to decouple, builds its kernel in
  `commitFormats`) and rewire via `setInput`/`linkOutput` at the infeasible wire.
- **Signalling is interface-complete but the trigger is implicit.** `twSpeaker`
  re-negotiates on every `startOutput`, so a device-rate or project-rate change
  is picked up at the next play without a cached-negotiation invalidation path.
  `renegotiationRequired` is reserved for that future caching.

### Commits

- `4ec5fe3` — phases 4-5 (project rate + rate-aware core).
- `68d7814` — phase 6 (capabilities).
- `9d653c5` — phase 7 (negotiator + signalling).

### Next actions

1. **You: audible test.** Launch, `File → New` (project defaults to 48 kHz),
   add a track, import a sample, Play — confirm correct pitch on a 48 kHz
   device. A matched 48/48 project should log the resampler as passthrough; a
   loaded legacy 44.1 kHz project should log `resampling 44100 Hz -> 48000 Hz`.
2. **Linux ALSA smoke build** — still outstanding from earlier phases; the
   converter port to ALSA was not compiled here.
3. **Optional UI** — no widget exposes the project sample rate yet; add a
   project-settings control if user-facing rate changes are wanted.
4. **Finish healing** when a fixed-rate source lands (see deferred fork above).
