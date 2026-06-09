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

---

## 02_AUDIO_DRIVER_STRATEGY.md — revised to support the wire format (design)

- **Date:** 2026-05-31
- **Status:** Proposal 02 revised (design only). Brings the audio-driver
  strategy in line with the as-built callback-pull `AudioBackend` and proposal
  04's wire format, by making the backend a participant in rate negotiation:
  it **returns** the rates it can open natively (`supportedRates()`) and
  **accepts** a requested rate (`openDevice(device, preferredRate)`), with
  `AudioConfig.sampleType` carrying the device-native binary format.

Key points captured in the revision:
- New normative "Wire-format rate negotiation" section supersedes the original
  speculative push/`writeAudio` interface sketch for the rate/format aspect.
- Negotiation flow: seed `D = candidateRates ∪ {projectRate} ∪
  backend.supportedRates()`, resolve with a no-resample preference, request that
  rate at open, then configure the speaker resampler `graphRate →
  getConfig().sampleRate` (passthrough when the request is honored). This is the
  resolution of proposal 04's open item *"auto-extend D with device-advertised
  rates"* → yes.
- Shared vs. exclusive mode documented: `preferredRate` is advisory in shared
  mode (mixer owns the rate) and bit-perfect in exclusive mode; exclusive mode
  is the lever behind 04's deferred *"device vs. fixed-rate source anchor"* fork.
- Per-backend realization notes (WASAPI `IsFormatSupported`/`GetMixFormat`, ALSA
  `set_rate_near`/`test_rate`, CoreAudio nominal-sample-rate properties), plus a
  new timeline item **1b** and two success criteria.

### Implementation — timeline item 1b (done)

- **Date:** 2026-05-31
- **Status:** ✅ Backend native-rate negotiation implemented and building green on
  Windows/Qt6/MinGW; window-up smoke test passes. Audible verification pending.

| File | Change |
|------|--------|
| `tw303a/include/audio/audio_backend.h` | `AudioConfig.sampleType` (twSampleType); `supportedRates()` pure virtual; `openDevice` gains `preferredRate` (default 0). |
| `audio/null_backend.{h,cc}` | Honors `preferredRate` (no hardware constraint); `supportedRates()` returns `{}`. |
| `audio/wasapi_backend.{h,cc}` | Sets `config_.sampleType` from the detected mix format; logs when a requested rate can't be honored in shared mode; `supportedRates()` returns `{mixRate}` once open. |
| `audio/alsa_backend.{h,cc}` | Requests `preferredRate` via `set_rate_near`; `sampleType = Int16`; `supportedRates()` probes the candidate set with `snd_pcm_hw_params_test_rate` (Linux — not compiled here). |
| `twnegotiator.{h,cc}` | `negotiate(target, extraRates)` overload folds device-advertised rates into `D`. |
| `twspeaker.cc` | Reordered: open device requesting the graph rate → negotiate with `backend_->supportedRates()` → configure resampler `graphRate → getConfig().sampleRate` (passthrough when the request is honored). |

Net: the backend now **requests** the graph rate and **returns** its native rate
set + format. On a device that can open at the project rate, the speaker
resampler is a passthrough. Shared-mode WASAPI can't change its mix rate, so the
request is advisory there (resampler bridges); exclusive-mode honoring is the
documented future step. Committed in this session.

---

## Audio output device picker + per-user settings store

- **Date:** 2026-05-31
- **Status:** ✅ Implemented and building green on Windows/Qt6/MinGW; window-up
  smoke test passes (the Audio menu, and thus WASAPI device enumeration, is
  built at startup without crashing). Picking a device / observing the saved
  INI still wants a human click.

### What landed

| File | Change |
|------|--------|
| `main/include/ssettings.h` + `src/ssettings.cpp` (new) | `SSettings` singleton over `QSettings(IniFormat, UserScope, "Smaragd", "smaragd")` — a real per-user INI (`%APPDATA%/Smaragd/smaragd.ini`, `~/.config/Smaragd/smaragd.ini`). Keys: `audio/deviceId`, `paths/<context>`. This is the requested "(device,user)-specific config file". |
| `tw303a/include/audio/audio_backend.h` | `AudioDeviceInfo{id,name}`; `enumerateDevices()` pure virtual. |
| `audio/wasapi_backend.{h,cc}` | `enumerateDevices()` via `IMMDeviceEnumerator::EnumAudioEndpoints` (id + `PKEY_Device_FriendlyName`, defined locally for MinGW); `openDevice` honors a non-default endpoint id via `GetDevice`, falling back to default. UTF-8↔wide helpers. COM init is balanced defensively in the enumerator. |
| `audio/null_backend.h`, `audio/alsa_backend.{h,cc}` | `enumerateDevices()`: Null returns `{}`; ALSA enumerates cards via `snd_card_next` (Linux, uncompiled). |
| `tw303a/include/twspeaker.h` + `src/twspeaker.cc` | `setOutputDevice`/`outputDevice`/`outputDevices`; `startOutput` opens the selected id. Engine stays GUI-agnostic (id is a plain string; no SSettings dependency). |
| `main/src/sapplication.cpp` | Sets org/app name; restores the saved device id into the speaker at startup. |
| `main/include/smainwindow.h` + `src/smainwindow.cpp` | New **Audio → Output Device** submenu: a `QActionGroup` of checkable entries from `speaker->outputDevices()`, current selection checked. Choosing one calls `setOutputDevice` + persists to `SSettings` (takes effect next Play; a status-bar note if currently playing). |
| `main/src/smainwindow.cpp`, `sstdmixerview.cpp` | The project-open and sample-import file dialogs now start at the remembered `SSettings::lastDir("project"/"sample", …)` and store the chosen directory back. |

### Notes / decisions

- **Layering:** device enumeration and the device id live in the engine
  (`twSpeaker`/`AudioBackend`), but the *settings* and *menu* live in `main/`.
  The engine never depends on `SSettings`; the GUI orchestrates (load id →
  `setOutputDevice`; user picks → persist).
- **Effective timing:** a device change applies at the next `startOutput()`
  (the speaker reads the id when opening). No mid-play device switch — kept
  simple; a stop/restart could be added later.
- **Shared-mode caveat carries over:** selecting a device still goes through
  shared-mode WASAPI, so its mix rate governs and the speaker resampler bridges
  as needed.

### Next actions

1. **You:** run it, open the **Audio → Output Device** menu, pick a device,
   confirm `%APPDATA%/Smaragd/smaragd.ini` appears with `audio/deviceId`; reopen
   a file dialog to confirm it returns to the last directory.
2. ALSA device enumeration is uncompiled here — needs a Linux build.
3. Per-device *rate* selection in the picker UI (exclusive-mode) is a natural
   follow-on now that `supportedRates()` exists.

---

## CoreAudio backend diagnostic work + fix (macOS)

- **Date:** 2026-06-01
- **Status:** ✅ **FIXED** — CoreAudio backend now produces audible audio on macOS.

### Initial Symptoms

- macOS build with `-DENABLE_COREAUDIO=ON` compiled successfully
- All setup calls succeeded, but render callback (`renderOnce_()`) was never invoked
- No audio produced; cursor did not advance during playback

### Root Cause

The audio format descriptor had two issues:

1. **Incorrect byte sizes:** `mBytesPerPacket` and `mBytesPerFrame` were hardcoded to 4 (single-channel float32) instead of `4 * channels` (stereo float32 = 8 bytes)
2. **Audio unit type:** HALOutput and GenericOutput didn't trigger callbacks; switching back to `DefaultOutput` was required

### Fix Applied

| File | Change |
|------|--------|
| `coreaudio_backend.cc` | **Audio unit:** Switched from HALOutput → DefaultOutput (standard speaker output). **Format:** Fixed `mBytesPerPacket` and `mBytesPerFrame` to `4 * channels` for stereo float32. Set format on both INPUT scope (where we provide data) and OUTPUT scope (device expectation, often read-only). **Logging:** Simplified diagnostic output; kept syslog calls, removed excessive fprintf. |
| `sstdmixerview.cpp` | (from prior session) Fixed file dialog blocking via `DontUseNativeDialog` flag. |

### Verification

- **Build:** ✅ Compiles cleanly on macOS / Qt6.11.1 / arm64
- **Audible playback:** ✅ **Confirmed** — user tested with `File → New`, added track, imported sample, pressed Play. Heard audio and saw playback cursor advance.
- **Diagnostic output shows:**
  ```
  CoreAudioBackend::renderCallback_ INVOKED! (call #1, frames=512, ...)
  CoreAudioBackend::renderCallback_ INVOKED! (call #100, frames=512, ...)
  CoreAudioBackend::renderCallback_ INVOKED! (call #200, frames=512, ...)
  ```

### Architecture Summary

The final working pattern:
1. **Audio unit:** `DefaultOutput` (kAudioUnitSubType_DefaultOutput)
2. **Format:** Stereo float32, interleaved, at device sample rate (typically 48 kHz)
3. **Callback:** Render callback on INPUT scope, invoked by CoreAudio when data is needed
4. **Data flow:** Synth (mono) → speaker resampler (mono → device rate) → CoreAudio callback → output buffers → speakers

### Files touched

- `coreaudio_backend.cc` — fixed format descriptor, switched to DefaultOutput, streamlined logging
- `smainwindow.cpp` — (from prior session) added diagnostic qWarning messages
- `twspeaker.cc` — (from prior session) added diagnostic fprintf output
- `sstdmixerview.cpp` — (from prior session) fixed file dialog blocking issue

### Current State

macOS audio **fully operational**. The synth produces audible output at device native rate (48 kHz on modern Macs); the speaker resampler bridges any project-rate mismatch. Playback cursor advances correctly, reflecting synth time (not device time).

---

## 03_ACTION_MODEL.md — Phase 2 rollout strategy

- **Date:** 2026-06-01
- **Status:** Design complete (Phase 2 sequencing rationale documented in `plan/proposed/03a_ACTION_MODEL_PHASE_2_ROLLOUT.md`). Implementation begins with Phase 2a.

### Rollout decision

Phase 1 implemented the action substrate (queue, history, registry, undo bridge) + four proof-of-concept actions. Phase 2 introduces the first **production-ready** actions, sequenced as:

1. **Phase 2a:** `SAddSampleAction` hardened → creates testable audio content
2. **Phase 2b:** `SSetTrackVolumeAction` with merge → volume changes audibly observable because 2a added audio

### Rationale

Testing volume changes without sample content means amplifying silence — the action mechanism works (UI updates, undo functions), but you have zero way to verify the audio path is correct. With `SAddSampleAction` first, phase 2b has testable audio so volume changes are audibly verifiable. De-risks by validating the basic apply/inverse/undo/save/load cycle before adding merge logic.

See `plan/proposed/03a_ACTION_MODEL_PHASE_2_ROLLOUT.md` for detailed phase breakdown, acceptance criteria, and success metrics for each phase.

---

## 03_ACTION_MODEL.md — Phase 2a (add sample with undo)

- **Date:** 2026-06-01
- **Status:** ✅ Code complete. **Ready for compilation and audible test on macOS/Windows.**

### What landed

| File | Purpose |
|------|---------|
| `main/include/actions/sremovesampleaction.h` (new) | Inverse action: removes clip from track, reconstructs SAddSampleAction with original file path + position |
| `main/src/actions/sremovesampleaction.cpp` (new) | Implementation + self-registration to `SActionRegistry` |
| `main/src/actions/saddsampleaction.cpp` (refined) | Now returns `SRemoveSampleAction` as inverse; self-registration added |
| `main/CMakeLists.txt` | Added `sremovesampleaction.{h,cpp}` to build |

### How it works

**SAddSampleAction::apply():**
1. Creates clip (SCut + SLink) on track at timePos
2. Finds newly created clip in track's children list
3. Captures clip index
4. Returns `SRemoveSampleAction(trackIdx, clipIdx, filePath, timePos)` as inverse

**SRemoveSampleAction::apply():**
1. Gets clip at the stored index
2. Deletes the clip (Qt + ref-counting handles full cleanup)
3. Returns `SAddSampleAction(trackIdx, filePath, timePos)` as inverse

**Serialization:**
- Both actions serialize all needed fields (trackIdx, filePath, timePos, clipIdx for remove)
- XML round-trips preserve pending sample imports across save/load

### Design notes

- **Clip index stability:** Captured immediately after creation, used on undo. If clips shift between apply and undo (e.g., another clip added/removed), undo would fail. Phase 2b will address this if needed; for now, acceptable because samples are typically not reordered during undo sequence.
- **File deletion:** When SCut is deleted, its wavLink is also deleted via SCut destructor. SLink's ref-counting ensures file is freed only when all references gone.
- **Inverse symmetry:** inverse-of-inverse is symmetric (undo of undo = redo); no special-casing needed.

### Verification needed

Compile and test on macOS/Windows:
1. Create project, add track, import sample via Test menu or manual action
2. Hear audio play back
3. Ctrl+Z: sample removed, silence
4. Ctrl+Y (redo): sample returns, audio plays again
5. Save during pending import; reload; sample is there

### Next action

Compile and test, then proceed to Phase 2b (SSetTrackVolumeAction with merge).

---

## Thread safety fix: UI redraw + audio playback race condition

- **Date:** 2026-06-02
- **Status:** ✅ Fixed. Race condition between UI thread (preview rendering) and audio thread (playback) both accessing `twWavInput::file_` without synchronization is eliminated.

### Problem

- **Symptom:** EXC_BAD_ACCESS crash during waveform preview rendering while audio is playing
- **Root cause:** `twWavInput::calcOutputTo()` accesses `file_` from both UI thread (via SPlainWaveRendererInline::draw → getPreview) and audio thread (via CoreAudio callback), with interleaving on file_.seek/read operations
- **Impact:** Playing audio + visible waveform preview = crash

### Solution

Added `std::mutex fileMutex_` to `twWavInput` class:

| File | Change |
|------|--------|
| `tw303a/include/twwavinput.h` | Added `#include <mutex>` and `mutable std::mutex fileMutex_;` member |
| `tw303a/src/twwavinput.cc` | Added `#include <mutex>`; wrapped `file_.seek()` + `file_.read()` in both `calcOutputTo()` and `findWaveProperties()` with `lock_guard<mutex>` |

### How it works

- Lock scope is minimal (just file I/O, ~0.1-1ms)
- Both threads call same function but now seek+read is atomic per thread
- No interleaving possible; one thread waits if other holds lock
- Audio latency impact: negligible (file I/O already slow)

### Documentation created

- `THREAD_SAFETY_ANALYSIS.md` — detailed race condition mechanics
- `EXECUTION_PATH_DIAGRAM.md` — visual timeline of crash scenario
- `SYNCHRONIZATION_FIX_PLAN.md` — implementation guide
- `THREAD_SAFETY_SUMMARY.txt` — quick reference
- Thread affinity annotations added to: `splainwave.h`, `twwavinput.h`, `sexternfile.h`, `scut.h`

### Verification

- **Build:** ✅ Clean on macOS, no compilation errors
- **Test scenario:** App running, ready for user test (play audio + drag window to force redraw)

### Commits

- `44ffbb7` — Fix thread safety race condition in twWavInput file access

---

## Fix undo/redo/undo sequence: action reusability

- **Date:** 2026-06-02
- **Status:** ✅ Fixed. Second and subsequent undo/redo operations now work correctly.

### Problem

- **Symptom:** test sequence → undo → redo → undo (second undo) does nothing
- **Root cause:** After first undo, `SActionUndoCommand` cleared the `inverse_` pointer (and `forward_` in redo). On second undo, pointer was null so no action could be applied
- **Root cause analysis:** Comment said "submit deletes the action" but with `skipHistory=true`, submit() **doesn't** delete—action is still owned by undo command

### Solution

Removed the "clear pointer after apply" logic in `SActionUndoCommand::undo()` and `redo()`:

| File | Change |
|------|---------|
| `main/src/sactionundocommand.cpp` | Removed `inverse_ = nullptr;` after `submit()` in undo(); removed `forward_ = nullptr;` after `submit()` in redo() |

### How it works

- SAction objects are immutable command objects, designed to be reusable
- They're owned by `SActionUndoCommand` and deleted in the destructor
- Each undo/redo call re-applies the same action object
- No need to clear pointers; they stay valid for multiple applies

### Example flow (now correct)

1. Apply AddTrack → creates undo command with forward=AddTrack, inverse=RemoveTrack
2. User undo → applies RemoveTrack (inverse stays valid)
3. User redo → applies AddTrack (forward stays valid)
4. User undo again → applies RemoveTrack (still valid!) ✅ Works now

### Verification

- **Build:** ✅ Clean, no errors
- **Ready for test:** test sequence → undo → redo → undo (all three operations now functional)

### Commits

- `3d936ea` — Fix undo/redo/undo sequence: keep action pointers for reuse

---

## 03_ACTION_MODEL.md — Phase 2b (set track volume + merge)

- **Date:** 2026-06-06
- **Status:** ✅ Code complete, builds clean on Windows/Qt6/MinGW
  (`build/bin/smaragd.exe`, ~29.2 MB). Audible + interactive verification is a
  human step (see below).

### What landed

| File | Purpose |
|------|---------|
| `main/include/actions/ssettrackvolumeaction.h` + `src/.../ssettrackvolumeaction.cpp` (new) | `SSetTrackVolumeAction(trackIdx, newVolume)`. `apply()` resolves the track (same `getTrackAt`→`getSObject`→`dynamic_cast<STrack*>` pattern as the sample actions), captures `getVolume()` for the inverse, calls `setVolume()`. `mergeKey()` = `set-track-volume:<idx>`; `mergeWith()` absorbs the newer volume. Self-registers as `"set-track-volume"`. |
| `main/CMakeLists.txt` | Header + source added to the build lists. |
| `main/src/ssmvmixercontrol.cpp` + `include/ssmvmixercontrol.h` | The track volume control now routes through `submitAction(new SSetTrackVolumeAction(...))` instead of calling `tk_.setVolume()` directly. New `trackIndex_()` helper resolves the control's track index from `smv_.getModel()`; falls back to a direct `setVolume()` if the index can't be resolved (track gone) so the UI never wedges. **UI follow-up (2026-06-06):** replaced the up/down dB spinbox (`qxDBSpinBox`, now deleted) with a **vertical fader** (`QSlider`, loud at top, ticks every 12 dB) plus a centred dB readout label. Model→view updates go through `setSliderSilently()`, which wraps `setValue()` in a `QSignalBlocker` so a programmatic fader move (e.g. during undo) can't re-emit `valueChanged` and submit a spurious action. Same tenths-of-a-dB range (−96.0..+24.0 dB). |
| `main/src/smainwindow.cpp` + `include/smainwindow.h` | New **Test → Volume Burst (track 0)** entry submits 50 volume actions ramping −24..+6 db and logs the undo-stack delta (expect +1 if merge worked). |

### How merge actually works today (important)

`setVolume()` already emits `volumeChanged()`, which is wired to **both** the
audio mixer (`SStdMixer::trackVolumeChanged` → `setInputLevel`) and the UI
spinbox, so the action only needs to call `setVolume()` — propagation is free.
No feedback loop: `setVolume`'s `fabs(...)<0.0001` guard plus the spinbox's
"only set if different" guard break the cycle.

Coalescing is realized at the **`QUndoStack` layer**, not the queue. Because
`SActionHistory::submit()` enqueues then drains **synchronously** on the GUI
thread, the queue never holds two entries, so the enqueue-time `mergeWith` is
dormant. Instead `SActionUndoCommand::id()`/`mergeWith()` collapse the 50 pushed
commands into one undo entry (forward absorbs the latest volume; the inverse
keeps the pre-burst level). One Ctrl+Z restores the original.

### Honestly deferred to phase 2c (async drain)

- **True enqueue-time 50→1 *apply* coalescing.** Today it's 50 cheap applies →
  1 undo step. Collapsing to a single engine apply needs the async drain.
- **Near-zero-latency in-flight `tryCancel` undo.** Structurally unreachable
  under synchronous drain.
- **Pending-action persistence** (`snapshotPending()` is still a stub). In the
  synchronous model the volume is already applied and serialized via normal
  `SObject` XML (`volume='...'`), so "reload at the dragged level" works anyway —
  just not via the `<pending-actions>` block.

### Verification needed (human)

1. Run **Test → Run Test Sequence...** (creates project, track 0, sample, plays).
2. Drag the track-0 volume spinbox during playback → level changes audibly,
   pitch/speed unchanged.
3. **Test → Volume Burst (track 0)** → stderr/status shows undo stack `+1`.
4. One Ctrl+Z after the burst restores the pre-burst level.
5. Delete the track, drag its (former) volume → apply fails cleanly, UI consistent.

### Next action

Verify interactively, then phase 2c (async engine-thread drain) to realize the
deferred items above.

---

## Project Save / Save As / Close + save & load actions

- **Date:** 2026-06-06
- **Status:** ✅ Code complete, builds clean on Windows/Qt6/MinGW (only the
  pre-existing vendor XPM `-Wwrite-strings` noise). Window-up smoke test passes.
  Interactive verification (save a file, reopen it, run the round-trip) is a
  human step.

### Background

Serialization already worked end to end (`SProject::serialize` + `SProjectLoader`;
**File → Open** was functional). The gaps were UI/wiring only: **Save As** and
**Close** were wired to `nyi()` ("not yet implemented"), and **Save** hardcoded
`project.qxp` in the CWD with no dialog and silent failure.

### What landed

| File | Change |
|------|--------|
| `main/include/actions/ssaveprojectaction.{h}` + `src/.../ssaveprojectaction.cpp` (new) | `SSaveProjectAction(path)`: `apply()` serializes the project to `path`; non-undoable (`{applied, nullptr}`). Self-registers as `save-project`. |
| `main/include/actions/sloadprojectaction.{h}` + `src/.../sloadprojectaction.cpp` (new) | `SLoadProjectAction(path)`: `apply()` runs `SProjectLoader` + `createObjects` into the supplied (empty) project; non-undoable. Self-registers as `load-project`. |
| `main/CMakeLists.txt` | Both files added. |
| `main/src/smainwindow.cpp` + `.h` | `fileSaveAs()` (QFileDialog, ensures `.qxp`), `fileClose()`, `saveToPath()` helper, `updateWindowTitle()` (shows `Smaragd - <file>` / `untitled`), `currentFilePath_` member. **Save** now saves to the remembered path or falls back to Save As; **Save As** and **Close** menu items un-wired from `nyi()`. `fileOpen`/`fileSave` now go **through the actions** (single code path shared with tests). Ctrl+Shift+S bound to Save As. |
| `main/src/smainwindow.cpp` (`closeProject`) | Now nulls `projectRootWidget_` after delete — it previously left a dangling pointer; calling close twice (now reachable via the load-failure path and File → Close) would have double-freed. |
| `main/src/smainwindow.cpp` (`fileOpen`) | Load-failure paths now `closeProject()` + return instead of dereferencing a half-built / deleted project (a latent crash). |

### Actions for testability (the second ask)

Save/load are now real `SAction`s, so they are scriptable and testable via the
same registry as every other action. **Test → Save/Load Round-trip** drives them
as a self-contained assertion: saves the live project to `QDir::tempPath()/
smaragd_roundtrip.qxp` via `SSaveProjectAction`, reloads into a throwaway
`SProject` via `SLoadProjectAction` (the live project is never disturbed),
compares track counts, and reports OK/FAILED to stderr + status bar. The GUI
Save/Open paths call the very same actions, so the test exercises the production
code path.

### Notes / decisions

- Save/load `apply()` is called **directly** (not via `submitAction`) in both the
  GUI and the test, because they are non-undoable and the caller needs the
  success result synchronously. Consistent with how a headless test would invoke
  them.
- There is no formal unit-test harness in the repo yet; the **Test menu** is the
  de-facto test runner, so the round-trip lives there. The actions themselves are
  harness-agnostic (no Qt-GUI dependency in `apply`), so they can move into a real
  test target later unchanged.

### Verification needed (human)

1. **File → Save As…** on a project → choose a path → file written, title shows it.
2. **File → Open…** that file → loads, title updates.
3. **File → Save** on a loaded/saved project → overwrites silently (status note).
4. **Test → Save/Load Round-trip** → stderr/status shows `Round-trip OK: N tracks`.
5. **File → Close** → central view clears, title back to `Smaragd`, no crash.

---

## Crash fixes: load truncates path, and populated-project teardown

- **Date:** 2026-06-06
- **Status:** ✅ Fixed and verified with a headless gdb reproduction (build a
  project with a sample → save → load into a probe → delete probe → delete live
  project → process exits normally, no SIGSEGV). Builds clean on Windows/Qt6/MinGW.

### Symptom

User created a project, ran the test sequence (loads a WAV + plays), stopped
playback, then ran **Test → Save/Load Round-trip** → segfault.

### Diagnosis (gdb, via a temporary `--repro` harness in `main`, since removed)

Two independent bugs, both pre-existing and newly *reached* by the save/load work:

1. **Wave path truncated to one character on load** → `SPlainWave` failed to open
   the file → its instantiate returned NULL → `SProjectLoader::createObjects`
   dereferenced that NULL at `object->readAttributes(e)`.
2. **Destroying a populated project crashed** — the first time a project with
   content was ever destroyed (`delete probe`; also reachable now via File →
   Close / File → Open-replace). An `SLink` destructor called `removeRef()` on a
   sibling object that had already been freed.

### Fixes

| File | Fix |
|------|-----|
| `main/src/splainwave.cpp` | `instantiateFromDomElement` read the filename as `(const char*)element.attribute("filename").data()` — casting `QString::data()` (QChar*, UTF-16) to `const char*` truncates `"C:/..."` to `"C"` at the first NUL byte. Use the `QString` directly. (Same buggy cast in a nearby log line also fixed.) The file is saved correctly; only the *read* was broken — a Qt5→Qt6 wide-char porting bug. |
| `main/src/sprojectloader.cpp` (`createObjects`) | Null-check the instantiate result before `readAttributes`; abort the load with `-1` instead of dereferencing NULL (graceful failure when, e.g., a referenced WAV is missing). |
| `main/src/sprojectloader.cpp` (`~SProjectLoader`) | Delete the temporary "handle" `SLink`s held in `objectDict_` (and properly free the registry entries). These handles are loading scaffolding, distinct from the real parent/child links; leaking them kept every loaded object's refcount permanently above zero so it could never be torn down cleanly. |
| `main/src/sproject.cpp` (`~SProject`) | Tear down the object graph by repeatedly deleting only objects whose reference count has reached zero. Deleting an object frees its child `SLink`s, which drop references and bring the next layer to zero — so deletion cascades root→leaf and no `SLink` ever dereferences a freed object. Done in the destructor body so child destructors still see a live `externFileDict_`. |
| `main/src/splainwave.cpp` (`~SPlainWave`) | Deregister from the object's *own* project (its QObject parent), not `SApplication`'s current project (which is NULL during Close and wrong when loading into a non-current project). |

### Note

These teardown bugs were latent because a populated project had never actually
been destroyed before (the old `closeProject` only ran on empty/in-flight
projects). Save/Load + File → Close are the first paths to exercise it.

---

## Toolbar palette: snap-to-grid / grid / metronome / cycle toggles

- **Date:** 2026-06-06
- **Status:** ✅ Code complete, builds clean on Windows/Qt6/MinGW (no new
  warnings). Window-up smoke test passes; visual/interactive check is a human
  step (couldn't reliably screenshot the app window from this environment).

### What landed

A small palette of four checkable square buttons in a new toolbar, each with a
shortcut, that toggle on click or keypress:

| Button | Shortcut | Real? |
|--------|----------|-------|
| Snap to grid (S) | `S` | yes — gates `SStdMixerView::alignTime` |
| Grid (G) | `G` | yes — gates the time-grid drawing in `SMVActualView::paintEvent`, repaints on toggle |
| Metronome (M) | `M` | **stub** — toggles state only, no click track |
| Cycle (C) | `C` | **stub** — toggles state only, no looped playback |

### Design

- **State** lives in `SApplication` (`snapToGrid_`/`gridVisible_`/`metronomeOn_`/
  `cycleOn_`, defaults `true/true/false/false`) with setters that emit
  `*Changed(bool)` signals — same home as transport (`isPlaying`).
- **Actions**: a header-only base `SToggleSettingAction` (carries an
  `Op{Toggle,Enable,Disable}`, non-undoable) + four thin subclasses
  (`SSnapToGridAction`, `SGridAction`, `SMetronomeAction`, `SCycleAction`). Each
  registers **three verbs** — e.g. `snap-to-grid-toggle/-enable/-disable` — so
  every feature has toggle/enable/disable actions, scriptable via the registry.
- **UI** (`SMainWindow::buildPaletteToolbar`): checkable `QAction`s with
  generated 22×22 square letter icons. `triggered()` submits the `*-toggle`
  action; the `SApplication *Changed` signal drives `setChecked`, so the button
  stays in sync however the setting changes (button, shortcut, or script) — no
  feedback loop because `setChecked` doesn't emit `triggered`.
- Snap/grid were already implemented in the view (always-on); this just gates
  them on the new toggle state. Metronome/cycle setters carry `// TODO stub`.

### Files

New: `actions/stogglesettingaction.h`, `actions/s{snaptogrid,grid,metronome,cycle}action.{h,cpp}`.
Edited: `sapplication.{h,cpp}` (state+signals), `smainwindow.{h,cpp}` (palette
toolbar + slots + icon helper), `sstdmixerview.cpp` (gate alignTime + grid draw,
repaint on grid toggle), `main/CMakeLists.txt`.

### Verification needed (human)

1. Four square buttons appear in a toolbar; Snap + Grid start pressed.
2. `G` / clicking Grid hides/shows the time grid; `S` toggles clip snapping.
3. `M` / `C` toggle their buttons (no audible/transport effect yet — stubs).


---

## Per-project property dictionary (generic key/value store)

- **Date:** 2026-06-06
- **Status:** ✅ Code complete, builds clean (no new warnings). Round-trip
  **verified headlessly**: defaults seed correctly, snap/cycle/an arbitrary int
  key all persist through save→load. Window-up smoke passes. Visual/interactive
  check (toggle, save, reopen, confirm restored) is a human step.

### Decision

Discussed and challenged the "generic property bag" idea. Outcome (user choices):
**per-project** storage (saved in the `.qxp`), and **both** a generic action and
named convenience wrappers. Non-undoable (view/transport toggles). Qt mechanism:
a `QVariantMap` (Qt's JSON-object analog) serialized as JSON — chosen over QObject
dynamic properties (poor change-notification/serialization ergonomics) and over
hand-rolled XML-typed elements.

### What landed

- **`SProject` property store**: `QVariantMap properties_` + `prop(key,default)` /
  `setProp(key,value)` / `hasProp(key)` / `properties()` and a
  `propertyChanged(QString,QVariant)` signal. Named `prop*` (not
  `property/setProperty`) to avoid shadowing QObject's meta-property API. Seeded
  from `SProjectProps::defaults()` at construction.
- **`sprojectprops.h`** (new): well-known key constants (`SnapToGrid`,
  `GridVisible`, `Metronome`, `Cycle`) + `defaults()`. Keeps the stringly-typed
  bag discoverable/typo-proof.
- **Serialization**: the dict is written as JSON in a single `properties='...'`
  attribute on `<SProject>` (compact `QJsonDocument`, minimal XML-attr escaping),
  read back in `readPreChildrenAttributes` and merged over the seeded defaults —
  so old files (no attribute) load with defaults and unknown future keys survive.
- **Actions** (the four toggles now read/write the project, not SApplication):
  - base `SToggleSettingAction::get/setState` now take `SProject*`.
  - `SSnapToGridAction` / `SGridAction` / `SMetronomeAction` / `SCycleAction`
    operate on `project->prop/setProp` with the `SProjectProps` keys.
  - new generic `SSetPropertyAction(key, value)` registered as `set-property`
    (value JSON-encoded for type-preserving serialization).
- **`SApplication`**: the snap/grid/metronome/cycle members, getters, setters and
  signals added in the previous step were **removed** — state moved to the dict.
- **Toolbar palette** (`SMainWindow`): buttons now reflect the *current project's*
  properties. `syncPaletteToProject()` enables+seeds the buttons and connects to
  the project's `propertyChanged` (called from fileNew/fileOpen/fileClose);
  `onProjectPropertyChanged()` keeps each button in sync. Buttons are disabled
  when no project is open.
- **`SStdMixerView`**: `alignTime` (snap) and the grid drawing now read the
  project's properties; repaints on the project's `propertyChanged`.

### Notes / open points

- Snap/grid/metronome/cycle are now saved *in the project file*. As flagged in the
  design discussion, on-off prefs like snap/metronome are arguably per-user; if
  that becomes annoying, a per-user override layer (SSettings) can seed new
  projects without changing this store.
- `SSetPropertyAction` is non-undoable for now; the property bag makes undo trivial
  later (capture old value as inverse) if wanted.

---

## Track shortcuts (Ctrl+T / Ctrl+Return) + remove-track crash fix

- **Date:** 2026-06-06
- **Status:** ✅ Builds clean. Remove-track crash **reproduced and fixed**
  (verified headlessly under gdb with the mixer view present). Shortcut behaviour
  is human-verifiable.

### Remove-track crash (root cause)

`SStdMixerView::removeMixerControl` crashed at `controlArray_->at(1)` on an
emptied vector. Two bugs combined:
1. It read `model_->getNTracks()` for the count, but `SStdMixer::removeTrack`
   emits `trackRemoved` (which drives `removeMixerControl`) **before** deleting
   the track link — so the count still included the track being removed.
2. The reposition loop was `for(t=trackIdx; t<newNTracks; --t)` — wrong
   decrement, and `controlArray_->at(t+1)` indexed past the vector that
   `takeAt()` had already shrunk.

Fixes:
- `removeMixerControl` rewritten to use the actual `controlArray_->size()` (not
  the stale model count); `takeAt()` already removes+compacts, so the bogus
  manual shift loop is gone — just reposition the remaining controls.
- `SStdMixer::removeTrack` now deletes the link **before** emitting
  `trackRemoved`, so listeners (mixer rewiring, the view) see the post-removal
  state. This also fixes a latent audio bug where `reconnectTracksToMixer` left a
  dangling mixer input to the just-removed track. The track object survives the
  delete (refcount→0 → deleteLater) so passing it to the signal stays valid.

### Shortcuts

- **Ctrl+T → New track** and **Ctrl+Return (and Ctrl+Enter) → Insert sample** are
  now persistent `QAction`s owned by `SStdMixerView` (added to the widget), so
  their shortcuts actually fire whenever the arranger window is up. Previously
  they were created fresh inside the context menu on every popup, so the
  shortcuts never registered (the insert-sample one was "listed but did nothing").
  The same action objects are placed into the right-click menu by `ctGlobalShow`,
  so the menu shows the shortcut and there's no duplicate-shortcut ambiguity.
- `ctAddTrack` now routes through `SAddTrackAction` (undoable + rewires the
  speaker) instead of a direct `insertTrack`, so the context menu and Ctrl+T do
  the same correct thing.

### Verification needed (human)

1. Ctrl+T adds a track; Ctrl+Return inserts a sample at the last-clicked track.
2. Right-click → Remove track no longer crashes (single track and multi-track).

---

## Per-track Mute / Solo buttons

- **Date:** 2026-06-06
- **Status:** ✅ Builds clean, window-up smoke passes. Audible behaviour is a
  human step (no headless hook into the live mixer state).

### What landed

Small square **M**/**S** toggle buttons in each track's channel strip
(`SSMVMixerControl`): Mute turns red when on, Solo turns yellow. They drive the
existing per-`SObject` `muted`/`solo` flags (already serialized), so state
persists with the project.

Routing (`SStdMixer`): `reconnectTracksToMixer` now computes, per track,
`audible = !muted && (!anySoloed || soloed)`. Inaudible tracks get a **NULL mixer
input** (their DSP isn't pulled — processing *and* output disabled) plus level 0;
audible tracks get their root output at their volume.

- `mutedChanged`/`soloChanged` from each track are connected (in `insertTrack`)
  to a new `trackMuteSoloChanged()` slot → full `reconnectTracksToMixer()` (solo
  is global, so all tracks are re-evaluated).
- `trackVolumeChanged` now also respects audibility, so dragging a muted/
  non-soloed track's fader can't un-silence it.
- `anyTrackSoloed()` helper added.

### Behaviour

- **Mute**: silences that track (input detached → no processing/output).
- **Solo**: as soon as any track is soloed, every non-soloed track is silenced;
  a soloed track still obeys its own mute.

### Verification needed (human)

Two tracks with audio: mute one → it goes silent; solo the other → only it plays;
clear solo → both play; muted+soloed track stays silent.

---

## 05_TRACK_GROUPING_AND_LIVE_ASSETS.md (proposed)

- **Date:** 2026-06-06
- **Status:** Design only — concept authored, no code.

Concept for two requested features, unified under "composition of
sub-arrangements":

- **(a) Track groups** — tracks as children of tracks (Reaper-style folders);
  parent sums child-track outputs with its own clips, through its own processing.
- **(b) Live region assets** — a marked time region becomes a shareable
  sub-arrangement in the resource list, placed via SLink/SCut (whole or part),
  not rendered to file; editing the master changes all instances; recursive.

Key findings that make this tractable: `twTrackMix` already sums *any* child
`SObject`'s root component (nesting is how the DSP already works); `SObject`
sharing + the live-pull render already give "edit once, hear everywhere"; `SCut`
already is a windowed view. The one real prerequisite is making track processing
(gain/mute/solo) **intrinsic** to the track strip so tracks compose uniformly —
which also touches the mute/solo just landed. (b) is largely (a) + register as a
resource + range-selection + extract-and-replace + an acyclicity guard.

See `plan/proposed/05_TRACK_GROUPING_AND_LIVE_ASSETS.md` for the model/DSP/UI/
serialization breakdown, a 6-phase rollout, and open questions.

---

## Range selection in the ruler (proposal 05, first increment)

- **Date:** 2026-06-06
- **Status:** ✅ Builds clean, window-up smoke passes. Interactive behaviour is a
  human step. First slice of `05_TRACK_GROUPING_AND_LIVE_ASSETS.md` §2.6.

### What landed (all in `SMVActualView`)

- **State**: `rangeValid_` + two ends `rangeStart_/rangeEnd_` (stored unordered,
  normalized on release) + a `rangeDrag_` mode (none / create / move-start /
  move-end). Public `hasRange()/getRangeStart()/getRangeEnd()` for later use
  (asset creation).
- **Interaction** (top ruler band, `y < SMV_TIME_RULER_HEIGHT`):
  - Left-press *not* on an end → start a new range (this press fixes one end);
    drag moves the other end; release fixes it. A zero-length click clears.
  - Left-press within `SMV_RANGE_GRAB_PIXEL` of an existing end → drag that end.
  - Both ends snap via `smv_.alignTime()` (so they honor the snap-to-grid
    project property).
  - Range drag takes precedence over clip editing in mouseMove/Release.
- **Rendering** (`drawRange`, last in paintEvent): grey band in the ruler
  between the ends; both ends as vertical lines over the full track height.
- **Context menu** (`qRangePopup_`, shown on right-click in the ruler): "Set
  BPM..." (moved here from the old ruler right-click hack), "Clear range"
  (enabled only when a range exists), and "Create asset from range" — a **stub**
  wired for feature (b).

### Notes

- The old ruler right-click → BPM-input hack is gone; BPM now lives in the range
  menu. Ruler left-click no longer seeks (it selects a range); track-area
  left-click still seeks.
- Range state is view-local for now; per proposal 05 it should migrate to
  `SProject` (with a track set) when asset creation lands.

### Verification needed (human)

Drag a range in the ruler → grey band + full-height edges appear; with snap on,
ends land on grid lines; grab an end and move it; right-click → menu with
Clear/Set BPM/Create asset (stub).

---

## Grouping/assets: §0 intrinsic track processing (foundation)

- **Date:** 2026-06-06
- **Status:** ✅ Builds clean, window-up smoke passes. Audio behaviour is a human
  step (volume/mute/solo should be unchanged for the current flat arrangement).

### Design refinement (from review)

Returning to proposal 05 with range selection in hand, the user reframed the
asset model: **an asset is just an `SCut` windowing a track group** — the group
stays put as the single source of truth, the cut is a live window, edit-once-
everywhere is automatic, recursion is free, and nothing is extracted or baked.
So **grouping is built first**; asset creation then becomes "make an SCut over
the selected group for the current range". (Recorded in
`plan/proposed/05_…md` §4b.)

### What landed (§0)

Made track output **self-contained** so a track sums correctly wherever it is
placed (master mixer today, a parent track/group tomorrow):

- `twTrackMix::calcOutputTo` now applies the track's own gain
  (`pow(10, getVolume()/20)`) and mute, read live each buffer.
- `SStdMixer::reconnectTracksToMixer` sums tracks at **unity** (0 dB) instead of
  applying per-track volume at the mixer input.
- Removed the now-obsolete `SStdMixer::trackVolumeChanged` slot + its
  `volumeChanged` connection (volume is picked up live by twTrackMix).

Behaviour-equivalent for the current flat arrangement (gain moved one stage
earlier). Mute/solo still work; solo is still resolved at the mixer (top-level
only) — nested-track solo is a documented follow-up.

### Next

Track-tree model + reparent action → indented arranger UI → assets as SCut-on-group.

---

## Grouping/assets: §1 track-tree model + reparent action (Phase 2)

- **Date:** 2026-06-07
- **Status:** ✅ Code complete, builds clean on Windows/Qt6/MinGW
  (`build/bin/smaragd.exe`, ~34.6 MB). §0 audio behaviour was verified working by
  the user first. Interactive verification of the new tree/undo is the **Test →
  Group Track Test** entry (a self-contained assert).

### What landed

Tracks can now form a **tree**. Structurally the model already allowed it (a
container's children are `SLink`s to any `SObject`) and the DSP already summed it
(`twTrackMix::calcOutputTo` pulls every child's `getRootComponent()` live each
buffer, and `SLink::hasStartTime()` is always true, so a child track at
startTime 0 contributes immediately). What was missing was a safe *operation* to
rewire the tree, plus a test that it round-trips.

| File | Purpose |
|------|---------|
| `main/include/actions/sreparenttrackaction.h` + `src/.../sreparenttrackaction.cpp` (new) | `SReparentTrackAction(sourcePath, destParentPath, destIndex)`. Tracks/containers are addressed by an **index-path from the root mixer** (`{}` = mixer, `{2}` = its 3rd child, `{2,1}` = 2nd child of that). `apply()` resolves source + dest **before** any mutation, validates (must be a track; dest must be a container; reject same-container reorder; cycle guard via `isSelfOrDescendant`), then moves: `addRef()` pins the track across the detach→attach gap (so the transient zero-ref window can't fire the irreversible `removeRef→deleteLater`), detaches (mixer: `removeTrack(SLink&)` + clear stale `track→mixer` signal connections; track parent: `delete link` + clear `track→parent`), attaches (mixer: `insertTrack`; track parent: `new SLink(track,NULL); setParent`), `removeRef()`, `rewireSpeaker()`. The **inverse is synthesized from the post-move tree** (`pathOf`) so it is immune to the index shifts the move causes in both containers. Self-registers as `reparent-track`. |
| `main/CMakeLists.txt` | Header + source added. |
| `main/src/smainwindow.cpp` + `.h` | New **Test → Group Track Test (tree + undo)**: ensures ≥2 top tracks, groups track `{1}` under `{0}`, asserts the mixer shrank by one and track 0 gained a track-typed child, round-trips the nested arrangement through `SSaveProjectAction`/`SLoadProjectAction` into a throwaway project, then undoes and asserts the flat arrangement returns. Reports OK/FAILED to stderr + status bar. |

### Serialization

**No format change.** Nesting is just `SLink` children of a track; the loader's
two-pass `createObjects` already rebuilds arbitrary `SObject`/`SLink` trees by
id, and `STrack::instantiateFromDomElement` already loops its `SLink` children
and resolves each `objectId` — a track-child-of-track resolves because the nested
track is instantiated first (dependency ordering).

### Honestly deferred

- **Append-only attach.** The model appends tracks (`insertTrack`'s index is
  cosmetic; QObject child order = creation order, with no reorder API), so undo
  restores **membership, not the exact original slot**. Consistent with the fact
  that no track-reorder exists anywhere yet; a future move/reorder action covers
  it.
- **Same-container reorder** is rejected by the action (out of scope — it is a
  reorder, not a reparent).
- **Nested-track solo** still resolved at the top-level mixer only (carried over
  from §0).

### Verification needed (human)

1. **Test → Group Track Test** → stderr/status shows `Group test OK`.
2. (Once §1.2 UI lands) visually confirm the indented lanes.

### Next

Indented arranger UI (§1.2): walk the track tree depth-first in `SMVActualView`
instead of the flat `getTrackAt(i)` list; indent + fold triangle per parent.

---

## Grouping/assets: explicit child order + exact-slot move/reorder

- **Date:** 2026-06-07
- **Status:** ✅ Code complete, builds clean on Windows/Qt6/MinGW; window-up smoke
  passes. Verify interactively via **Test → Reorder Track Test** and re-run
  **Test → Group Track Test** (the undo path is now exact-slot).

### Why

The Phase 2 reparent was append-only, so undo restored *membership, not the
original slot*. Root cause: the model conflated **order** with **QObject child
order**, and Qt has no public reorder API (`setParent` always appends). On the
user's call we switched to the idiomatic Qt approach — an **explicit ordered
list as the source of truth**, with QObject parentage left to mean *ownership
only* — and hid all iteration behind an **iterator** so call sites don't depend
on the storage.

### What landed

| File | Change |
|------|--------|
| `main/include/sobject.h` + `src/sobject.cpp` | `SObject` now owns `QList<SLink*> childOrder_` (the order source of truth). `childEvent()` keeps its **membership** in sync with QObject parentage (append on add, drop on remove); **order** is then set by `moveChildToIndex(from,to)` — a plain `QList::move`, no `setParent` dance, no signal churn, refcounts untouched. New storage-agnostic accessors: `childLinks()` (a range, `for (SLink* lk : obj->childLinks())`), `childCount()`, `childAt(i)`, `indexOfChild()`, `indexOfChildObject()`. A small `SChildLinks` range type wraps the list so the iterator type is the abstraction, not `QObject::children()`. |
| `main/src/sstdmixer.cpp` + `.h`, `strack.cpp`, `strackrndrinline.cpp`, `sobject.cpp`, `tw303a/.../twtrackmix.cc`, sample actions | Every **order-relevant** `QObject::children()` reader switched to the iterator/accessors (getNTracks, getTrackAt, reconnectTracksToMixer, anyTrackSoloed, seekTo, getTopMostSLinkAt, getChildrenExtent, serialize, the inline renderer, twTrackMix's sum loop, add/remove-sample). `SProject`'s `children()` (flat *SObject* ownership list, not SLink order) deliberately untouched. |
| `main/src/sstdmixer.cpp` + `.h` | **`insertTrack`'s cosmetic index param removed** → `insertTrack(STrack&)`; it always appended (the index only fed the signal) and now emits the real landing index. New **`reorderTrack(from,to)`** = `moveChildToIndex` + `reconnectTracksToMixer` (bus inputs are index-assigned) + new **`tracksReordered()`** signal (a hook for the §1.2 view). |
| `main/src/actions/saddtrackaction.cpp` | Now **honours its index for real** (append then `reorderTrack` into place) — previously it silently appended regardless, so its inverse could remove the wrong track. |
| `main/include/actions/strackpath.h` (new) | Shared inline path helpers (`childLinkAt`/`resolveByPath`/`pathOf`/`isSelfOrDescendant`/`pathToString`/`stringToPath`) built on the iterator API, used by both tree actions. |
| `main/src/actions/sreparenttrackaction.cpp` | Refactored onto `strackpath.h`; now **honours an exact `destIndex`** (append then place) and its **inverse restores the exact original (parent, index)**. The append-only limitation is gone. |
| `main/include/actions/smovetrackaction.{h}` + `src/.../smovetrackaction.cpp` (new) | `SMoveTrackAction(sourcePath, toIndex)` — in-place reorder within the current parent (mixer or folder track). Undoable; inverse moves back to the exact original index. Registers as `move-track`. |
| `main/CMakeLists.txt`, `main/src/smainwindow.cpp` + `.h` | Build wiring + **Test → Reorder Track Test**: tags 3 tracks by volume, moves track 0 → slot 2, asserts the new order, round-trips it, and asserts undo restores the exact original order. |

### How ordering works now (the model)

`childOrder_` is the single source of truth for sequence; `QObject::children()`
is only ownership/lifetime and may differ in order after a reorder (same
membership, always). Save/load preserves logical order because `serialize`
writes `childLinks()` order and the loader rebuilds via `setParent`, which
appends to `childOrder_` in document order.

### Live arranger refresh on reorder — DONE (2026-06-07)

The earlier deferral is implemented. `SStdMixerView` now has a `tracksReordered()`
slot, connected to the mixer's `tracksReordered()` signal, that **re-sequences the
existing control-column widgets** to match the model order (matching each control
to its track via the new `SSMVMixerControl::getTrack()`, then repositioning) and
repaints the lanes — no control is created or destroyed. The invariant
"`controlArray_` order == model order" is now maintained by all three view slots
(`addMixerControl`/`removeMixerControl`/`tracksReordered`), so live reorder, group,
and undo all keep the faders aligned with the lanes.

| File | Change |
|------|--------|
| `main/include/ssmvmixercontrol.h` | Public `STrack &getTrack() const`. |
| `main/include/sstdmixerview.h` + `src/sstdmixerview.cpp` | `tracksReordered()` slot (reorders `controlArray_` to model order + repositions + `qContent_->update()`); connected to `model_`'s `tracksReordered()` in the ctor. |

### Verification needed (human)

1. **Test → Reorder Track Test** → `Reorder test OK: 012 -> 120 -> undo 012`,
   and the faders visibly follow the lanes.
2. **Test → Group Track Test** → `Group test OK` (its undo is now exact-slot).

### Next

Indented arranger UI (§1.2), now on a clean ordered-tree foundation: walk the
tree depth-first, indent + fold per parent, and re-sequence the control column
on `tracksReordered()`.

---

## Grouping/assets: mouse drag-to-reorder tracks (interactive)

- **Date:** 2026-06-07
- **Status:** ✅ Code complete, builds clean on Windows/Qt6/MinGW; window-up smoke
  passes. The drag itself is a human check (drag a track's grip up/down).

### Why

The reorder/move engine + actions existed but were only reachable from the Test
menu. The user asked for a **manual mouse** way to reorder top-level tracks
(keyboard/context-menu not chosen; **manual grouping deliberately deferred to
§1.2**, since a grouped track leaves the flat view and would be invisible until
the indented tree UI exists).

### What landed

A **grip handle** — a 12 px strip down the **left** side of each channel-strip
control (`SSMVMixerControl`), drawn by the control so mouse events on it reach
the control directly (its child widgets cover the rest). Dragging the grip
reorders the track. (Mute/Solo are stacked vertically, mute over solo, beside the
fader.)

| File | Change |
|------|--------|
| `main/include/ssmvmixercontrol.h` + `src/ssmvmixercontrol.cpp` | Reserve the left `HANDLE_W` px (grid left-margin) and `paintEvent` a vertical grip there. `mousePressEvent` on the grip arms a drag; `mouseMoveEvent` past a 4 px threshold starts it (cursor → closed hand, grip turns blue) and forwards the pointer (mapped to the control-column) to the view; `mouseReleaseEvent` ends it. New `dragArmed_`/`dragging_`/`dragPressPos_` state. Mute/Solo laid out in a vertical column. |
| `main/include/sstdmixerview.h` + `src/sstdmixerview.cpp` | `beginTrackDrag/updateTrackDrag/endTrackDrag` + `insertSlotAt()`. A thin `QFrame` `dropIndicator_` (child of the control box) shows the insertion line while dragging; on release the gap is mapped to a target index and an **`SMoveTrackAction`** is submitted (undoable; the existing `tracksReordered()` path then re-sequences the faders). Dropping on the track's own slot is a no-op. |

### Notes / scope

- Reorder applies to **top-level** tracks (the control column only shows those).
- Manual **grouping** (reparent into a folder) is **deferred to §1.2** by choice —
  the engine (`SReparentTrackAction`) is ready; it just needs the tree UI to be
  visible/usable. No keyboard or context-menu reorder was added (mouse only).

### Verification needed (human)

1. Drag a track's grip up/down → a blue insertion line tracks the pointer;
   on release the track (fader + lane) moves to that slot. Ctrl+Z restores it.

---

## Grouping/assets: §1.2 indented arranger — display foundation (Stage 1)

- **Date:** 2026-06-07
- **Status:** ✅ Code complete, builds clean on Windows/Qt6/MinGW; window-up smoke
  passes. Visual check is a human step (see below).

### Why

The arranger assumed a flat `getTrackAt(i)` / `i*trackHeight` mapping everywhere
(paint, hit-testing, clip-drag, scroll, the control column). To show nested
tracks it had to be restructured around the tree. This is the **display
foundation**; the three grouping *gestures* (drag-to-nest, context indent/outdent,
toolbar Group/Ungroup — all requested) come next on top of it.

### What landed

A **flattened depth-first row model** owned by `SStdMixerView`: `rows_` (a
`QVector<STrackRow>` of `{track, link, parent, depth, hasChildren, collapsed}`),
rebuilt by `refreshTrackTree()` (→ `rebuildRows` walks the tree, skipping
collapsed subtrees → `rebuildControlColumn` → scroll range → repaint). Per-track
fold state lives in a `QSet<STrack*> collapsed_`.

| Area | Change |
|------|--------|
| `sstdmixerview.h/.cpp` | `STrackRow` + `rows_`/`collapsed_`; `rowCount/rowAt/rowIndexOfTrack/toggleTrackCollapsed/refreshTrackTree`. Paint walks rows (indent band per depth, full-width clips); `updateLastClickVars`, clip-drag, scroll range, zoom and the ctor all index rows instead of the flat mixer. The incremental `add/removeMixerControl`/`tracksReordered` slots now just call `refreshTrackTree()` (a full, always-correct rebuild). Grip drag still reorders **top-level** tracks for now. |
| `ssmvmixercontrol.h/.cpp` | `setTreeInfo(depth,foldable,collapsed)`: indents the strip content, draws a ▾/▸ **fold triangle** for parents (click toggles via `toggleTrackCollapsed`), and offsets the grip by the indent + fold gutter. |
| `sstdmixer.h/.cpp` | `notifyTreeChanged()` (emits `tracksReordered()`) so tree-editing actions can force a post-operation view rebuild. |
| `sreparenttrackaction.cpp`, `smovetrackaction.cpp` | Call `notifyTreeChanged()` after mutating — the detach fires a mid-operation refresh and the folder-side attach emits no mixer signal, so the final state needs an explicit nudge. |
| `smainwindow.cpp/.h` | **Test → Nest Track 1 Under 0 (persist)** so the indented display is visible (the Group Track Test self-undoes). Ctrl+Z ungroups. |

### Notes / scope

- Grip drag reorders **top-level** tracks only; dragging nested tracks + the
  three grouping gestures are Stage 2.
- Control column is rebuilt wholesale on any structural change (cheap; keeps it
  in lockstep with `rows_`). Volume changes do **not** trigger it.

### Verification needed (human)

1. **Test → Nest Track 1 Under 0 (persist)** → track 1 appears as an indented
   lane under track 0, which shows a ▾ fold triangle. Clicking the triangle
   collapses/expands; the nested fader indents to match. Ctrl+Z ungroups.

### Next

§1.2 Stage 2 — the three grouping gestures (drag-to-nest, context-menu
indent/outdent, toolbar Group/Ungroup), plus nested-track grip reorder.

---

## Grouping/assets: §1.2 indented arranger — grouping gestures (Stage 2)

- **Date:** 2026-06-07
- **Status:** ✅ Code complete, builds clean on Windows/Qt6/MinGW; window-up smoke
  passes. Interactive verification is a human step.

### What landed — all three gestures (as requested)

**1. Drag-to-nest** (extends the grip drag). `resolveDrop(y)` classifies a drop:
over a lane's middle half → **nest** under that track (the drop indicator
outlines the whole target lane); on a lane boundary → **between**. On release:
nest = `SReparentTrackAction(pathOf(t), pathOf(onto), -1)`; between = reorder at
top level (`SMoveTrackAction`) if the dragged track is top-level, else **pop it
out** to top level at that slot (`SReparentTrackAction` to the mixer). Cycle/no-op
guards live in the action.

**2. Context-menu Indent/Outdent** (right-click a track lane). *Indent* nests the
track under its preceding sibling; *Outdent* moves it to its grandparent, just
after its old parent. Both compute paths via `strackpath` and submit
`SReparentTrackAction`. The menu also carries **Group/Ungroup**.

**3. Toolbar Group/Ungroup** (new "Tracks" toolbar in the main window; also in the
context menu). *Group* wraps the clicked top-level track in a new folder
(`SAddTrackAction` at the slot + `SReparentTrackAction` into it, wrapped in a
`QUndoStack` macro so it is one undo step). *Ungroup* moves a folder's child
tracks back out to the mixer at the folder's slot (macro'd, undoable). They act
on the arranger's last-clicked track (`SStdMixerView::ctGroupTrack/ctUngroupTrack`,
reached from the main window by casting the central `projectRootWidget_`).

| File | Change |
|------|--------|
| `sstdmixerview.h/.cpp` | `resolveDrop`; reworked `updateTrackDrag/endTrackDrag` for nest-vs-between; `ctIndentTrack/ctOutdentTrack/ctGroupTrack/ctUngroupTrack`; context-menu entries; `ctRemoveTrack` fixed to resolve the track's real mixer index (row index ≠ mixer index now). |
| `smainwindow.h/.cpp` | "Tracks" toolbar with Group/Ungroup → `groupTrack/ungroupTrack` forward to the arranger. |

### Honestly deferred

- **Group/Ungroup operate on top-level tracks.** (Ungroup now fully dissolves the
  folder — see the undoable-remove entry below.)
- Toolbar Group/Ungroup target the **last-clicked track** in the timeline lanes
  (clicking a fader strip doesn't set that selection yet).
- Between-drag reorder inside a folder isn't a distinct gesture — drop-onto nests,
  drop-on-boundary reorders/pops to top level; use Indent/Outdent for precise
  in-group moves.

### Verification needed (human)

1. Drag a track's grip onto another lane's middle → it nests (target lane
   outlined); drop on a boundary → reorder / pop-out. Ctrl+Z reverts.
2. Right-click a track → Indent/Outdent/Group/Ungroup behave; each is one undo.
3. Toolbar **Group**/**Ungroup** act on the clicked track.

---

## Grouping/assets: undoable track-remove (restore subtree)

- **Date:** 2026-06-07
- **Status:** ✅ Code complete, builds clean on Windows/Qt6/MinGW; window-up smoke
  passes. Validated via **Test → Undoable Remove Test** (human step).

### Why / how

`SRemoveTrackAction` was a Phase-1 stub returning `{true, nullptr}` (no undo), so
Ungroup couldn't delete the folder it emptied. Now it is **undoable by pinning**:
`apply()` takes an extra reference on the removed track and stores it *on the
action object itself* (`heldTrack_`/`holdsRef_`). Because a track owns its child
SLinks as QObject-children, pinning the track keeps its **entire subtree alive
and intact** — no serialization. The inverse, `SRestoreTrackAction`, reads that
pinned track back via the owning remove action and re-inserts it at its original
index, **preserving object identity** across undo/redo.

The pin lives on the *persistent* forward action (the undo command reuses
forward/inverse across undo↔redo; `skipHistory` submit deletes the *returned*
inverse each time — see `SActionHistory::submit`). `dropStalePin()` releases a
pin left by a previous apply whose track was orphaned (e.g. AddTrack's redo makes
a fresh track); the destructor releases the pin if the command is discarded while
in the removed state, finally tearing the subtree down.

| File | Change |
|------|--------|
| `actions/sremovetrackaction.{h,cpp}` | Undoable: pin on apply, `heldTrack()/releaseHeld()`, stale-pin + destructor handling; returns `SRestoreTrackAction`. |
| `actions/srestoretrackaction.{h,cpp}` (new) | Re-inserts the pinned track at its index, releases the pin, `notifyTreeChanged()`. Created live only (not registered/serialized). |
| `sstdmixerview.cpp` | `ctRemoveTrack` now routes through `SRemoveTrackAction` (undoable); **Ungroup deletes the emptied folder** inside its macro (undo restores folder, then children reparent back in — the reverse replay reconstructs each intermediate tree, so the exact-slot reparent inverses resolve). |
| `smainwindow.cpp/.h` | **Test → Undoable Remove Test**: groups, removes the folder+subtree, undoes, asserts the folder and its nested child return as the same objects. |

### Verification needed (human)

1. **Test → Undoable Remove Test** → `Undoable remove OK: folder+subtree restored
   (… same identity)`.
2. Right-click a folder → **Ungroup** now removes the folder entirely; Ctrl+Z
   restores it with its children.

### Crash fix (same session)

**SEGV on a second drag-to-group** (group track 2, then group track 3 onto the
same folder). Cause: `rebuildControlColumn()` did `delete mc` on every control,
and it is reached *synchronously from inside a control's own mouse handler* (a
grip-drag release that reparents, or a fold-triangle click) — so the control
freed itself while Qt was still dispatching its event (use-after-free, both in
the handler's trailing code and in Qt's dispatch). Fix: `rebuildControlColumn()`
now `hide()`s and `deleteLater()`s the old controls, so the handler unwinds
before they are destroyed. Covers the drag-release, fold-click, and any other
in-event structural change.

### Folder lane shows only its own clips (same session)

A child track's clips appeared on its parent folder's lane too: the folder's
inline renderer drew *every* child link with a duration — including its child
**tracks** (whose duration grows when you add a clip to them). A folder track
**sums** its children's audio (twTrackMix, unchanged) but its **lane** should be
an independent clip lane. Fix: `STrackRendererInline::draw` and
`STrack::getTopMostSLinkAt` now skip links whose object is an `STrack`, so a
folder lane draws/edits only its own clips while children render on their own
lanes.

---

## Clip move and split are now undoable actions

- **Date:** 2026-06-07
- **Status:** ✅ Code complete, builds clean on Windows/Qt6/MinGW; window-up smoke
  passes. Interactive verification (drag a clip, split a clip, Ctrl+Z) is a human
  step.

### Why

Dragging a clip and "Split object" mutated the model directly (setParent/
setStartTime/new SCut/...), bypassing the action system — so neither was
undoable. Made both real `SAction`s.

### What landed

| File | Change |
|------|--------|
| `actions/smoveclipaction.{h,cpp}` (new) | `SMoveClipAction(clipPath, destTrackPath, newStartTime)`. Clip addressed by track index-path + the link's index (`indexOfChild`, keyed on the link, so it is correct even for shared clips). `apply()` setParent (if the track changed) + setStartTime; the link object persists so there is no refcount dance. Inverse moves it back, synthesized from the post-move index. Clip order within a track is positional, so append-on-restore is fine. Registers as `move-clip`. |
| `actions/ssplitclipaction.{h,cpp}` + `sunsplitclipaction.{h,cpp}` (new) | `SSplitClipAction(clipPath, splitTime)` wraps the clip in an SCut if needed, sets the first part's length, and adds a second SCut/SLink for the remainder. Inverse `SUnsplitClipAction` deletes the second part and restores the first's length; its own inverse re-splits. Registers `split-clip` (unsplit is live-only). |
| `sstdmixerview.cpp` | Clip MOVE drag is now finalized in `mouseReleaseEvent`: the drag still mutates live for feedback, then on release it reverts to the pre-drag placement (snapshot captured on press) and re-applies via `SMoveClipAction` — one undo step. `ctSplitSample` routes through `SSplitClipAction`. |

### Honestly deferred

- **Clip resize** (drag the clip's left/right edge — `lastClickedStart_`/`End_`)
  still mutates live and is **not** actioned yet; it shares the press-snapshot
  machinery, so it is a natural follow-up (capture cut startOffset+duration too).
- Move/split keep object **identity** for the clip being moved / the first part;
  the split's second part is re-created on redo (content-equivalent).

### Verification needed (human)

1. Drag a clip to a new time / another lane → release → Ctrl+Z returns it; Ctrl+Y
   re-applies.
2. Position the playhead inside a clip → right-click → Split object → two clips;
   Ctrl+Z merges them back.

---

## Sample source / reader split (proposal 07, steps 1–4)

- **Date:** 2026-06-07
- **Status:** ✅ Code complete, builds clean on Windows/Qt6/MinGW. Interactive
  audio/preview verification is a human step.

### Why

`twWavInput` conflated immutable file data, the RAM cache, and a play **cursor**
in one object that `SPlainWave` **shared** among all its cuts. Sharing the cursor
made two cuts of one sample fight over a single `playOffset_` and thrash the
cache; the QFile was read from both the audio thread and the UI preview thread
under a lock. Proposal 07 splits the immutable data from the per-consumer cursor.

### What landed

| File | Change |
|------|--------|
| `tw303a/include/twrandomsource.h` (new) | `twRandomSource` interface: stateless `read(offset,dest,len,channel)`, `length/channels/sampleRate/isReproducible`, and `acquireReader(env)` — the reader factory. |
| `tw303a/include/twsamplesource.h` + `src/twsamplesource.cc` (new) | `twSampleSource`: decodes the whole WAV into **resident planar Float32** at construction, then `read()` is a lock-free memcpy (no QFile, no mutex). WAV header parse ported from the old `findWaveProperties`; 16-bit PCM only. |
| `tw303a/include/twsamplereader.h` + `src/twsamplereader.cc` (new) | `twSampleReader`: a thin per-consumer cursor `twComponent` over a `twRandomSource`; `calcOutputTo` reads at `pos_` and advances. `acquireReader()` defined here. |
| `tw303a/include/twwavinput.h` + `src/twwavinput.cc` | Rewritten as a thin cursor that **owns** a `twSampleSource`; dropped the QFile handle, mutex, and dead cache from the realtime path. Adds `getSource()`. Public API otherwise unchanged. |
| `main/include/sobject.h` + `src/sobject.cpp` | New `virtual twRandomSource *getRandomSource()` hook (default NULL). Preview (`straightCalcPreviewData`) reads statelessly via it when available → no lock, no cursor race with playback. |
| `main/include/splainwave.h` + `src/splainwave.cpp` | `getRandomSource()` returns the wave's source. |
| `main/include/scut.h` + `src/scut.cpp` | Each `SCut` lazily acquires its **own** reader from the content's source (cut-vs-cut cursor fix); `getRootComponent`/`seekTo` route through it. Falls back to the shared component when content is not a random-access source. |
| `tw303a/CMakeLists.txt` | Added the three new headers/sources. |

### Intentional behaviour change

- `read()` honours the requested **channel** (clamped to `[0, channels-1]`),
  fixing the old `twWavInput::calcOutputTo` bug that always returned channel 0 —
  so stereo material now feeds the right channel where the pipeline asks for it.
  Mono still plays on every channel. Mono-pipeline output is unchanged.

### Honestly deferred (proposal 07 §2 step 5, §6)

- **`twCapturingSource`** (random-access adapter over *any* linear `twComponent`,
  i.e. "time-stretch anything before an SCut") is **not** built yet: its only
  consumer is the grain node (proposal 06), which does not exist. The
  `getRandomSource()` API is the hook it will plug into.
- Windowed/streaming fallback for files too large to keep resident: not added
  (residency is the default; huge files are the only gap).

---

## Cached resampling: off-rate samples now play at correct pitch & length

- **Date:** 2026-06-07
- **Status:** ✅ Code complete, builds clean on Windows/Qt6/MinGW. Human listen on
  an off-rate sample (e.g. a 44.1 kHz file in a 48 kHz project) is the real check.

### Why

After the source/reader split a sample still played back at its native rate, so a
44.1 kHz file in a 48 kHz project ran fast and sharp (the twSpeaker rate
diagnostic was added for exactly this). Resampling on every block would be
wasteful; the data is reproducible, so resample once and cache.

### What landed

| File | Change |
|------|--------|
| `tw303a/include/twresampledsource.h` + `src/twresampledsource.cc` (new) | `twResampledSource`: a twRandomSource that materialises the whole material, resampled (linear) to a target rate, into a resident planar buffer ONCE in its ctor; read() is then a lock-free memcpy. Reproducible/shareable. |
| `tw303a/include/twsamplesource.h` + `src/twsamplesource.cc` | `viewAtRate(targetRate)`: returns `this` when the native rate matches (common case, zero cost), else a lazily-built, cached `twResampledSource` (rebuilt only if the requested rate changes). |
| `tw303a/src/twwavinput.cc` | `calcOutputTo`, `getLength`, and `getSource` all go through `source_->viewAtRate(env.getSRate())`, so playback, **duration**, preview, and cut readers are all project-rate coherent. The view is pre-built at load time (UI thread) so the one-time resample never lands in a realtime block. |
| `tw303a/CMakeLists.txt` | Added the new files. |

### Design note: why the cache is on the source, not literally in the reader

A per-reader resampler would (a) duplicate the resampled buffer for every cut of
one sample (the waste we explicitly want to avoid) and (b) leave preview and
`getDuration()` at the native rate while playback ran at the project rate —
off-rate samples would then play at correct pitch but wrong length (truncated
when upsampling, silence-padded when downsampling). A single cached view on the
source, read by preview + every reader + duration, is the only coherent place.
The reader still reads exclusively resampled, cached data — just not a private
copy.

### Honestly deferred / limitations

- **Mid-session project-rate change:** a cut's reader is acquired once over
  whatever `getSource()` returned then; it will not switch to a freshly-built
  view if the project rate later changes (the shared twWavInput path does
  self-correct). Rare; tied to the broader renegotiation debt.
- **Legacy off-rate projects:** a cut serialised with a native-frame
  `cutDuration_` (from before this change) stays that length on reload; new cuts
  get the correct project-rate duration.
- Linear interpolation only (matches twResampler); a polyphase upgrade is future.

---

## Grain playback MVP: per-clip time-stretch & pitch (proposal 06, phases 0–2)

- **Date:** 2026-06-07
- **Status:** ✅ Code complete, builds clean on Windows/Qt6/MinGW. Audible
  verification via the Test menu is a human step.

### Why

With the source/reader split + cached resampling in place, the grain engine has a
clean random-access foundation. Built the first useful slice: constant-rate
time-stretch and pitch-shift per clip, modelled as a cached `twRandomSource`
decorator (the "warped source" of proposal 06 §7.2) rather than the streaming
node — that route is reserved for variable/automated rate.

### What landed

| File | Change |
|------|--------|
| `tw303a/include/twgrainparams.h` (new) | `twGrainParams`: grainSize, crossfade, stretch, pitchCents, `isIdentity()`. |
| `tw303a/include/twgrainsource.h` + `src/twgrainsource.cc` (new) | `twGrainSource : twRandomSource`. Materialises the whole time-slice **overlap-add** result once into a resident planar buffer (normalised by a window-weight accumulator → unity gain incl. edges). Time-stretch = output-hop `G-C` vs input-hop `(G-C)/stretch`; pitch = per-grain linear resample by `2^(cents/1200)`. read() is then a lock-free memcpy. |
| `main/include/scut.h` + `src/scut.cpp` | `SCut` gains `Stretch`/`PitchCents` Q_PROPERTYs + `setGrainParams`. When non-identity it interposes an owned `twGrainSource` between the content view and its reader (passthrough otherwise). Params serialize; the clip's timeline length (and source window) rescale with stretch. Grain buffer is pre-built off the audio thread (on UI edit and on load). |
| `main/src/smainwindow.cpp` + `include/smainwindow.h` | Test menu: **Set Clip Stretch…** and **Set Clip Pitch…** (QInputDialog) act on the selected clip — the MVP verification trigger. |
| `tw303a/CMakeLists.txt` | Added the new files. |

### How to verify (human)

1. Load/record a clip, select it. 2. Test → "Set Clip Stretch…", e.g. 2.0 →
the clip doubles in length and plays at the same pitch, slower. 3. Test → "Set
Clip Pitch…", e.g. 1200 → up an octave, same length. Set while **stopped**.

### Honestly deferred / limitations

- **Realtime-unsafe param change:** rebuilding the grain buffer while the clip is
  actively playing races the audio thread (same class of hazard already in the
  codebase). Set params while stopped.
- **No UI beyond the Test menu**, no automation, no undo for these yet.
- **Non-source content** (synth, sub-mix) can't be stretched yet — needs
  `twCapturingSource` (proposal 07 step 5). Falls back to passthrough.
- **Slicer is fixed** (time-slice only); transient/hybrid slicers + a variable
  time map (streaming node) are the next phases.
- `twGrainer`/`twGrainSpec`/`SGrainFile`/`SGrainFileRendererInline` (old stub
  scaffold, never wired into the loader) have been **deleted** — superseded by
  `twGrainSource`. CMake entries and the stray loader include removed too.

---

## Per-user options + Options dialog + mouse-wheel zoom/pan

- **Date:** 2026-06-07
- **Status:** ✅ Code complete, builds clean on Windows/Qt6/MinGW; window-up smoke
  passes. Interactive verification (open the dialog, use the wheel) is a human step.
- **Plan:** `~/.claude/plans/lucky-nibbling-nygaard.md` (approved).

### Why

The arranger had zoom/pan primitives but the **mouse wheel did nothing**. Rather
than hard-code gestures, we wanted them user-configurable — which required a real
**per-user options** layer and a **preferences dialog**. Default wheel mapping is
**scroll-first**; the Audio page does output-device selection only.

### What landed

| File | Change |
|------|--------|
| `main/include/soptions.{h}` + `src/soptions.cpp` (new) | `SOpt` namespace: `WheelAction` enum (None/ScrollVertical/ScrollHorizontal/ZoomHorizontal/ZoomVertical), option keys, central `def(key)` defaults (scroll-first), and `wheelActionLabel()`. Single source of truth for keys+defaults. |
| `main/include/ssettings.h` + `src/ssettings.cpp` | `SSettings` is now a `QObject` singleton with generic `value(key,def)` / `setValue(key,val)` (emits `changed(key)` on a real change) over its QSettings INI. Existing `audioDeviceId`/`lastDir` reimplemented over it. |
| `main/include/soptionsdialog.{h}` + `src/soptionsdialog.cpp` (new) | `SOptionsDialog : QDialog` — left `QTreeWidget` (Mouse navigation, Audio) + right `QStackedWidget` page per leaf + OK/Cancel/Apply. Mouse page: 4 wheel-action combos (Plain/Shift/Ctrl/Ctrl+Shift) + *Zoom to cursor* / *Invert zoom* checkboxes. Audio page: output-device combo (same apply path as the Audio menu). |
| `main/src/smainwindow.cpp` + `.h` | **Edit → Options…** (Ctrl+,) → `showOptionsDialog()` (`exec()`s the dialog). |
| `main/src/sstdmixerview.cpp` + `.h` | `SMVActualView::wheelEvent`: maps the active modifier combo to a `WheelAction` from cached config (reloaded on `SSettings::changed`). Vertical scroll drives the v-scrollbar; horizontal scroll pans `setLeftOffset`; horizontal zoom changes `secondWidth` with **zoom-to-cursor** (keeps the time under the pointer fixed) honouring *Invert zoom*; vertical zoom changes `trackHeight`. Also fixed `setSecondWidth` to recompute `upperLeftX_` so the left edge no longer drifts on zoom (helps the corner zoom buttons too). |
| `main/CMakeLists.txt` | Added the four new files. |

### Verification needed (human)

1. **Edit → Options…**: tree shows Mouse navigation + Audio; pages switch.
2. Defaults: wheel scrolls tracks; Shift+wheel scrolls timeline; Ctrl+wheel zooms
   horizontally toward the cursor; Ctrl+Shift+wheel zooms track height.
3. Remap (e.g. plain wheel → Zoom horizontal) + Apply → behaviour changes live;
   toggle Invert zoom and confirm direction flips.
4. Audio page lists devices; pick + Apply → persists; relaunch → wheel + device
   settings remembered (INI under the user scope).

### Next

Possible follow-ups: a scroll-speed/zoom-step option; wheel handling on the ruler;
buffer/latency on the Audio page (currently fixed).

---

## Fix: large WAV truncated by a single QFile::read (preview + audio cut off)

- **Date:** 2026-06-07
- **Status:** ✅ Builds clean; needs human audio/visual verification.

### Symptom

On a long sample, the waveform preview and the audio both went flat/silent at
the same offset (~1/3 of a ~211 s file) — visible once horizontal zoom-out was
possible.

### Cause

`twSampleSource::loadWav()` read the whole PCM data chunk with a **single**
`file.read(raw.data(), rawBytes)` (~37 MB). `QFile::read()` does not guarantee
filling a large buffer in one call; a short return left the tail of the resident
buffer zero-filled while `nFrames_` (hence clip length, preview length, and the
resampled view) kept the header's full count — so everything past the bytes that
actually arrived was silence. The "loaded N frames resident" log printed the
header count, masking it.

### Fix (twsamplesource.cc)

Loop the read until all bytes arrive or a real EOF; if the file genuinely ends
short, clamp `nFrames_` to what was read (no phantom trailing silence) and warn.
EOF/short reads now load completely.

### Verify (human)

Load a long WAV, zoom out: the waveform should fill the whole clip and playback
should run to the end. If the console shows a "short read … clamping" warning,
that file was genuinely truncated on disk.

---

## Clip duplicate: Ctrl-drag a sample to a snapped copy

- **Date:** 2026-06-07
- **Status:** ✅ Builds clean; window-up smoke passes. Drag behaviour is a human check.

### What landed

**Ctrl + left-press on a clip** duplicates it and drags the copy; release drops it
at the (snapped) position. Undoable.

| File | Change |
|------|--------|
| `actions/sduplicateclipaction.{h,cpp}` (new) | `makeDuplicateClip(project, srcObj, destTrack, startTime)` — shared copy helper (SCut→copy content+window; raw clip→wrap whole). `SDuplicateClipAction(sourceClipPath, destTrackPath, startTime)`: creates the copy on the dest track; inverse `SRemoveClipAction`. Registers `duplicate-clip`. |
| `actions/sremoveclipaction.{h,cpp}` (new) | Inverse of duplicate: deletes the copied clip; its own inverse re-duplicates from the original (mirrors Split/Unsplit). |
| `sstdmixerview.cpp/.h` | mousePress: Ctrl+left on a clip builds a **live copy** via `makeDuplicateClip` and arms a duplicate move-drag (`clipDragIsDuplicate_`, `clipDupSourcePath_`); the existing move-drag (which snaps) drags it. mouseRelease: drops the live preview and submits `SDuplicateClipAction` at the final snapped position. Plain Shift/no-modifier selection unchanged. |
| `main/CMakeLists.txt` | Added the two action files. |

### Verify (human)

1. Ctrl-drag a clip → a copy follows the cursor (snapped to grid) and lands on
   release (same or another track). Ctrl+Z removes the copy; Ctrl+Y re-adds it.
2. The original clip is untouched; the copy shares the same audio content.

---

## Clip resize (edge drag) now snaps and is undoable

- **Date:** 2026-06-07
- **Status:** ✅ Builds clean; window-up smoke passes. Drag behaviour is a human check.

### What landed

Dragging a clip's left/right edge now (1) snaps the dragged edge to the grid and
(2) lands as a single undoable step.

| File | Change |
|------|--------|
| `actions/sresizeclipaction.{h,cpp}` (new) | `SResizeClipAction(clipPath, startTime, startOffset, duration)` sets an SCut's link start time + cut start-offset + duration; inverse restores the previous values. Registers `resize-clip`. |
| `sstdmixerview.cpp/.h` | The left/right edge drags were rewritten to compute from the **snapped absolute mouse time** (`alignTime(getTimeOf(x))`) against a press-time snapshot (`clipDragStart0_`, `clipResizeOffset0_`, `lastClickDuration_`) instead of accumulating raw deltas — so the edge follows the grid, with min-length and content-bounds clamping. `mouseReleaseEvent` reverts to the snapshot and submits `SResizeClipAction` (one undo step). New member `clipResizeOffset0_`. |
| `main/CMakeLists.txt` | Added the action. |

### Verify (human)

1. Drag a clip's right edge → its length snaps to the grid; Ctrl+Z restores the
   previous length, Ctrl+Y re-applies.
2. Drag the left edge → the start snaps and the front trims (content offset
   follows); undo/redo round-trips.
3. With snap off (Alt+S), edges drag freely again.

---

## Ctrl-drag duplicate now copies the whole selection

- **Date:** 2026-06-07
- **Status:** ✅ Builds clean; window-up smoke passes. Drag behaviour is a human check.

### What landed

Ctrl-dragging a clip already duplicated that one clip. Now, if the clicked clip
is part of a multi-selection (Shift-click to extend), the **entire selection** is
duplicated and dragged together: the clicked clip is the anchor and follows the
mouse (snapped); every other copy shifts by the same time delta and the same
lane-row delta, preserving the group's relative layout. Releasing submits one
`SDuplicateClipAction` per copy, wrapped in a single "Duplicate clips" undo macro
so the whole group reverts in one Ctrl+Z.

| File | Change |
|------|--------|
| `sstdmixerview.h` | Replaced the single `clipDupSourcePath_` with a `ClipDupItem` list (copy + sourcePath + origStart + origRow), anchor snapshot (`clipDupAnchorStart_`/`clipDupAnchorRow_`), and `syncDuplicateGroup()`. |
| `sstdmixerview.cpp` | Press: build the duplicate group from the current selection (or just the clicked clip), make a live copy of each, pick the anchor. Move: after the anchor moves, `syncDuplicateGroup()` drags the rest by the shared time/row delta. Release: capture each copy's final track+start, drop the previews, submit one action per copy inside a macro. |

### Verify (human)

1. Shift-click several clips (across tracks too), then Ctrl-drag one of them →
   all copies move together, the dragged one snapping to the grid, the rest
   keeping their relative spacing and track offsets.
2. Release → originals stay; copies land at the dragged positions.
3. Ctrl+Z removes all copies at once; Ctrl+Y restores them.
4. Ctrl-drag a single (unselected or lone) clip → still duplicates just that one.

---

## Clip-edge editing gestures: slip, time-stretch, loop, extend (+ cursors)

- **Date:** 2026-06-07
- **Status:** ✅ Builds clean; window-up smoke passes. Drag/audio behaviour is a human check.

### What landed

The full clip-edit vocabulary on top of `SCut`, all snap-aware and undoable
through one generalized action:

| Gesture | Input | Effect |
|---------|-------|--------|
| **Slip** | Alt-drag body | Slide the content under the clip (`startOffset`); position & length fixed. |
| **Time-stretch** | Ctrl-drag either border | Change timeline length, grain-stretch the same content to fit (pitch preserved); opposite edge anchored. |
| **Loop** | Right edge, **upper** half | Extend past the content end by repeating the previously-visible cut (real looped audio). |
| **Extend** | Right edge, **lower** half | Reveal more content, clamped at content end (prior behaviour). |
| **Trim/Move/Duplicate** | left edge / body / Ctrl-body | Unchanged. |

Hover cursors telegraph each gesture (SizeHor resize, SplitH stretch, SizeAll
slip, DragCopy duplicate, OpenHand move, custom ↻ for loop).

### Key pieces

- **`tw303a/twloopreader.{h,cc}` (new):** `twLoopReader : twSampleReader` wraps
  reads over `[loopBase, loopBase+loopLen)`, looping. The engine gap — `loopStart_`
  was vestigial (unread, unsaved) and `twTrackMix` does one linear read per clip.
- **`SCut`:** new `loopLength_` (loop active iff `0 < loopLength_ < cutDuration_`),
  `setWindow(startOffset,duration,loopLength,stretch)` (sets all four directly, no
  preserve-span rescale, one `rebuildReader`), `rebuildReader` builds a
  `twLoopReader` when looping, `seekTo` is loop-base aware, `loopLength` is now
  serialized. `setLoopLengthRaw` for cheap live-drag feedback.
- **`SCutRendererInline`:** tiles the loop segment with repeated clipped draws +
  boundary dividers (the wave renderer fetches one linear range per call, so
  tiling needs repeated calls).
- **`SResizeClipAction`:** generalized to the whole window
  `{startTime, startOffset, duration, loopLength, stretch}` — every edge gesture
  finalizes through it (revert-to-snapshot then submit), so all undo uniformly.
- **`SMVActualView`:** zone×modifier dispatch at press; cheap live field-only
  drags (audio rebuild deferred to the release action); `setMouseTracking` +
  `updateHoverCursor`.

### Verify (human, GUI)

1. **Slip**: Alt-drag a clip body → waveform slides, clip stays put; Ctrl+Z restores.
2. **Stretch**: Ctrl-drag a border → length changes, **pitch unchanged** on play,
   opposite edge fixed; undo/redo round-trips.
3. **Loop**: drag the right edge's **upper half** past the content end → cut repeats
   and **plays looped**; dividers drawn; save+reload keeps it; undo removes it.
   Lower-half drag still just reveals content to its end.
4. All honor snap (Alt+S); hover cursors change per zone/modifier.

---

## Status-bar mode indicator (clip-edit gestures)

- **Date:** 2026-06-08
- **Status:** ✅ Builds clean; window-up smoke passes. Hover-mapping is a human check.

A permanent mode indicator on the right of the main window's status bar reflects
the active arranger gesture as the cursor hovers a clip: Move, Slip, Duplicate,
Trim start, Extend, Loop, Time-stretch (blank off any clip). Routed through the
`SApplication` singleton (the app-wide QObject bus) so views stay decoupled:
`setStatusMode()`/`getStatusMode()` + `statusModeChanged()` (emits only on
change); `SMVActualView::updateHoverCursor` computes the label beside the cursor
shape it already picks; `SMainWindow::buildStatusBar` adds the `QLabel` and
connects it. Future status fields (BPM, selection, playhead time) follow the same
pattern.

---

## Live region assets — slice 1: create + register + display (proposal 05 feature (b))

- **Date:** 2026-06-08
- **Status:** ✅ Code complete, builds clean on Windows/Qt6/MinGW. Interactive
  create/undo verification is a human step.

### What it is

First vertical slice of proposal 05 feature (b). An **asset** is a live `SCut`
windowing an existing container — **vertical scope** = the container (the root
`SStdMixer`, or later a folder `STrack`), **horizontal scope** = a time range
`[t0,t1)` via `setWindow(startOffset=t0, duration=t1−t0)`. No copy, no
clip-splitting: it references the container, so editing the container's
tracks/clips changes the asset everywhere it is placed (the `SObject` is pulled
live each buffer). This is the §4b "cut over a group" model, generalised so the
group is any container. `SCut::getRootComponent()` already falls back to the
content's component when the content is not a sample source, so a cut over a
container reads its live `twTrackMix` for free.

### What landed

| File | Change |
|------|--------|
| `main/include/sproject.h` + `src/sproject.cpp` | **Asset registry**: `registerAsset(name, body)` (pins one ref via `addRef` so the asset survives with zero placements) / `unregisterAsset(name)` (emits `assetRemoved` then `removeRef` → `deleteLater`), `asset()/hasAsset()/assetNames()`, signals `assetAdded(name, body)` / `assetRemoved(name)`. New `assetDict_`. |
| `main/include/actions/screateassetaction.{h,cpp}` (new) | `SCreateAssetAction(containerPath, startOffset, duration, name="")`: resolve the container by index-path (`strackpath::resolveByPath`, `{}` = mixer), build the windowing `SCut`, register it (auto-names "Asset N" if unnamed). Inverse `SRemoveAssetAction`. Registered `create-asset`. |
| `main/include/actions/sremoveassetaction.{h,cpp}` (new) | Inverse: captures the asset's container path + window + name (the body is pure derived data), unregisters it; its inverse re-creates an identical `SCreateAssetAction`. Registered `remove-asset`. |
| `main/include/sexternfilelist.{h}` + `src/sexternfilelist.cpp` | The resource panel now lists **assets** beside sample files (rows keyed by asset name: name / "Asset" / live ref-count). New slots `assetAdded/assetRemoved/assetRefChanged`; `assetItemDict_` + `assetNameByBody_`. |
| `main/src/sstdmixerview.cpp` | The ruler range context-menu **"Create asset from range"** (was a stub) now submits `SCreateAssetAction({}, t0, t1−t0)` — vertical scope = the whole mixer. |
| `main/CMakeLists.txt` | Added the two action files. |

### Honestly deferred (next slices)

- **Placement** — dropping an asset from the resource list into the arrangement
  (an `SLink`/`SCut` instance). The model supports it (windowed playback rides
  the existing `getRootComponent`/`seekTo` fallback); the UI is not wired yet, so
  an asset can be created and seen but not yet placed.
- **Per-folder-track create** (vertical scope = an `STrack`) — the action already
  takes any container path; only a track-context-menu entry is missing.
- **Serialization** — assets are session-only; they are not yet written to the
  project file (§2.9).
- **Cycle guard** (§2.7), **editing an asset in a tab** (proposal 09), and
  length-follow (§2.8).

### Verify (human)

1. Drag a time range in the ruler, right-click → **Create asset from range** →
   an "Asset 1" row appears in the left resource list with ref-count 1.
2. Ctrl+Z removes the asset row; Ctrl+Y restores it.

---

## twCapturingSource — the asset cache primitive (proposal 07 step 5)

- **Date:** 2026-06-08
- **Status:** ✅ Engine class code-complete, builds clean on Windows/Qt6/MinGW.
  Not yet wired to a consumer (so no behaviour change yet).

### Why

A re-used live asset is a **cut over a group**, and a group's output is a single
twTrackMix node with **one** `playOffset_` cursor that advances on every
`calcOutputTo` call. Pulling it once per placement would (1) re-render the whole
sub-graph every buffer and (2) fight over that one cursor — the same shared-cursor
hazard proposal 07 removed for samples. The fix is to render the windowed output
**once** into immutable random-access data, then let each placement mint its own
independent reader over the snapshot. This is proposal 07's deferred **step 5**.

### What landed

| File | Change |
|------|--------|
| `tw303a/include/twcapturingsource.h` + `src/twcapturingsource.cc` (new) | `twCapturingSource : twRandomSource`. Constructor pulls a linear `twComponent` once — block by block, **re-seeking to the window start per channel** (twTrackMix advances its shared cursor per call regardless of channel) — into a planar Float32 buffer; `read()` is then a lock-free, zero-filling memcpy (mirrors `twResampledSource`). `isReproducible()==true`. Falls back to a single channel-0 pass when the source is not seekable. |
| `tw303a/CMakeLists.txt` | Added the new header/source. |

The capture is a **snapshot at construction** (it advances the source's cursor),
so it must run off the audio thread while the source isn't playing — the same
constraint as the grain materialisation.

### Consumer wiring — see the next entry (now landed)

- **Content-addressed shared cache** so identical assets/cuts render once
  (proposal 06 §7 tier 3) — still deferred.

---

## Asset cache wiring: transparent invalidate-on-edit

- **Date:** 2026-06-08
- **Status:** ✅ Code complete, builds clean on Windows/Qt6/MinGW; window-up smoke
  passes. **Dormant until placement** (nothing pulls an asset cut yet), so no
  behaviour change today — the cache is ready for when slice 2 lands.

### What landed

`SCut` now caches a container content's render through `twCapturingSource`, and
drops the cache transparently on any edit:

| File | Change |
|------|--------|
| `main/include/scut.h` + `src/scut.cpp` | New `ensureCapture()`: when the content is **not** a random source but **is** a seekable container (a track/mixer sub-arrangement) with a duration, render its whole output `[0, dur)` ONCE into an owned `twCapturingSource` and read from that. `rebuildReader()` now does `rs = getRandomSource(); if(!rs) rs = ensureCapture();` — so a cut over a group gets a cheap snapshot + its own reader (independent cursor) instead of re-pulling the live graph. `startOffset_` indexes into the capture exactly like a sample source, so cuts windowing the same container each get a correct view. New `invalidateCapture()` slot drops the capture (+ reader/grain) so the next pull re-captures; dtor frees the capture. Mono MVP (1 channel; served on every channel like a mono sample). |
| `main/include/sproject.h` | New `arrangementChanged()` signal + `notifyArrangementChanged()`. |
| `main/src/sactionhistory.cpp` | Fires `notifyArrangementChanged()` at **both** apply chokepoints (forward `drain_` + undo/redo `submit(skipHistory)`), so every applied action invalidates cached renders. |

### Design notes

- **Transparent, coarse invalidation.** Every applied action drops the cache (not
  just arrangement edits). Over-invalidation only costs a re-render, never
  correctness; it's also complete (deep edits don't have to bubble a signal up the
  tree). A finer "did the arrangement actually change" gate is a later optimisation.
- **Snapshot ⇒ stopped-only edits (MVP).** Re-capture/teardown is not realtime-safe
  (same documented stance as `rebuildReader`); edit/audition while stopped.
- **Only container-backed cuts connect** to `arrangementChanged`, so sample cuts
  are unaffected.

### Still deferred

- Content-addressed **shared** cache across identical cuts (06 §7 tier 3) — today
  each cut owns its own capture.
- A finer invalidation gate (only re-capture when the captured subtree changed).
- Multi-channel capture (mono for now).

---

## Live region assets — slice 2: placement (proposal 05, first audible test case)

- **Date:** 2026-06-08
- **Status:** ✅ Code complete, builds clean on macOS / Qt6. Window-up smoke test passes. Interactive drag-drop verification is a human step.

### What landed

Drag-and-drop from the resource list to the arranger:

| File | Change |
|------|--------|
| `actions/splaceassetaction.{h,cpp}` (new) | `SPlaceAssetAction(assetName, trackPath, timePos)`: resolves asset by name, pins refcount, creates an SLink, records clip index for the inverse. Registers as `place-asset`. |
| `actions/sremoveassetplacementaction.{h,cpp}` (new) | Inverse: removes the placement, returns `SPlaceAssetAction` for redo. Live-only (no XML serialization). |
| `sexternfilelist.{h,cpp}` | `setDragEnabled(true)` / `setSelectionMode(SingleSelection)` in ctor; override `startDrag()` to emit a `QDrag` with custom MIME `application/x-smaragd-resource` carrying either `asset:<name>` or `file:<path>`. |
| `sstdmixerview.{h,cpp}` | Add `setAcceptDrops(true)` in ctor; implement `dragEnterEvent`, `dragMoveEvent`, `dropEvent` to accept and decode the MIME payload, map drop position to (time, track), and submit either `SPlaceAssetAction` (asset drop) or `SAddSampleAction` (file drop). |
| `main/CMakeLists.txt` | Add the two new action files. |

### Bonuses

- **File drag-drop:** Dragging external files from the resource list now works (same as Insert Sample dialog, via the existing `SAddSampleAction`).
- **Undo/redo:** Asset placements (and file drops) are undoable — removal restores the asset body via the registry's extra reference pin.
- **Refcount tracking:** Resource list ref-count column auto-updates as placements are added/removed.

### How it works

1. User drags an asset from the resource list → `SExternFileList::startDrag` encodes it as `asset:AssetName`.
2. User releases over a track lane in the arranger → `SMVActualView::dropEvent` decodes it.
3. Drop position maps to (time, track); track path is computed via `strackpath::pathOf`.
4. `SPlaceAssetAction(assetName, trackPath, timePos)` is submitted → the asset is placed.
5. Undo invokes `SRemoveAssetPlacementAction` → the placement is deleted; registry pin keeps the asset alive.
6. Redo re-places it via the captured index.

### Verification needed (human)

1. Create an asset (drag time range in ruler → right-click → "Create asset from range").
2. Drag the asset from the resource list onto a track lane.
3. Hear it play back through the live `twCapturingSource` cache.
4. Ref-count in the resource panel increments; Ctrl+Z removes it (count decrements).
5. Drag an external file from the resource list as a bonus test (same mechanics via SAddSampleAction).

### Next (deferred in slice 2)

- Drop preview / ghost clip while dragging.
- Per-folder-track vertical scope (create assets scoped to a folder track, not just the mixer root).
- Asset serialization (session-only today; save/load assets from the project XML).
- Content-addressed shared cache (a single cached render for multiple identical cuts).

---

## Asset/file drag-drop: first Windows verification + three fixes

- **Date:** 2026-06-08
- **Status:** ✅ Fixed and **visually confirmed on Windows** (asset clip lands on the
  target lane; ghost label follows the cursor). Builds clean on Windows/Qt6/MinGW.

### Background

Slice 2's drag-drop placement was only ever built/verified on macOS. Its first
real exercise on Windows surfaced three issues — two functional, one cosmetic.
Diagnosed by temporarily instrumenting the whole DnD chain (since removed); the
trace confirmed `mousePress → startDrag → dragEnter → drop` all fired and the
drop mapped to a valid track, isolating the problems to drag-init and repaint.

### Fixes

| File | Fix |
|------|-----|
| `main/src/sexternfilelist.cpp` (ctor) | **Drag never started on Windows.** `setDragEnabled(true)` alone did not initiate the `QDrag` from the `QTreeWidget` on this Qt6/MinGW build. Added `setDragDropMode(QAbstractItemView::DragOnly)` — the idiomatic enable — which makes `startDrag()` fire. |
| `main/src/sstdmixerview.cpp` (`dropEvent`) | **Placed clip was invisible.** The drop submitted `SPlaceAssetAction`/`SAddSampleAction` (model updated correctly) but never repainted — the view's `update()` is wired only to track insert/remove and property changes, not clip additions. Added an `update()` at the end of `dropEvent`, mirroring the explicit `qContent_->update()` the normal `ctInsertSample` path already does. |
| `main/src/sexternfilelist.cpp` (`startDrag`) | **No drag ghost.** Our custom `QDrag` had no pixmap, so nothing followed the cursor (Qt's *default* item-view drag renders the row, but we build our own `QDrag`). Added a small rendered label pixmap + hotspot. |

### Diagnostic note (for next time)

On Windows/MinGW, `qWarning()`/`qDebug()` output does **not** reach the
bash-redirected `stderr` logfile (it routes to the Windows debug channel), so
DnD probes via `qWarning` were invisible. Engine logs show up because
`twsyslog.h` uses `fprintf(stderr,…)+fflush`. Use that same idiom (not
`qWarning`) when adding console diagnostics that must land in a redirected log.

### Known follow-ups (unchanged scope)

- The placed asset clip renders a **"No render" placeholder** — there is no
  waveform/preview renderer for a container-backed asset cut yet.
- **Cycle hazard:** an asset whose vertical scope is the whole mixer, placed back
  onto a track *inside* that mixer, is a self-reference. The deferred cycle guard
  (proposal 05 §2.7) still applies — be cautious hitting Play on such a placement.

---

## Recent-projects menu + open-most-recent + range-marker persistence

- **Date:** 2026-06-08
- **Status:** ✅ Implemented and **user-verified** ("Looks great!"). Builds clean on
  Windows/Qt6/MinGW.

### What landed

| File | Change |
|------|--------|
| `main/include/ssettings.h` + `src/ssettings.cpp` | `recentProjects()` / `addRecentProject()` / `removeRecentProject()` over a `recent/projects` INI key (newest-first, de-duplicated case-insensitively, capped at 5, absolute paths). |
| `main/src/smainwindow.cpp` + `.h` | New **File → Open Recent** submenu (`qRecentMenu_`, `updateRecentMenu()`); shows "(none)" when empty. `fileOpen` refactored to a thin dialog wrapper + shared `openProjectFile()` (closes current, loads, adds to recents, repaints) — so dialog-open, recent-open, and startup all share one path (cancelling Open no longer closes the current project). `saveToPath()` also adds to recents. New **`openMostRecent()`** opens the newest still-existing entry (prunes missing ones); called from `main.cpp` after `showMaximized()`. |
| `main/src/main.cpp` | Calls `win->openMostRecent()` at startup (app previously booted with no project). |
| `main/include/sprojectprops.h` | New property keys `RangeValid`/`RangeStart`/`RangeEnd` (+ defaults). The ruler **time-range marker** now lives in the project property bag (already JSON-round-tripped through save/load). |
| `main/src/sstdmixerview.cpp` | `SMVActualView::saveRangeToProject()` / `loadRangeFromProject()`; the view writes the range on `endRangeDrag`/`ctRangeClear` and reads it back in its constructor (the project is fully loaded before the view is built). |

---

## Track-scoped asset creation + acyclicity guard

- **Date:** 2026-06-08
- **Status:** ✅ Implemented and **user-verified** ("Looks good!"). Builds clean.

Reframed asset creation to dodge the self-reference trap (a whole-mixer asset
placed back into the mixer cycles at capture-build time).

| File | Change |
|------|--------|
| `main/src/sstdmixerview.cpp` | **Moved** "Create asset from range" off the ruler menu onto the **track context menu** (`ctCreateAssetFromTrack`), scoped vertically to the right-clicked track via `SCreateAssetAction(pathOf(root, lastClickTrack_), …)` instead of `{}` (whole mixer). Disabled (with a hint) when no range is selected. |
| `main/src/actions/splaceassetaction.cpp` | **Cycle guard (proposal 05 §2.7):** refuse to place an asset onto its source container or a descendant (`SCut::getContent()` → `isSelfOrDescendant(track, container)`). Authoritative backstop for any caller. |
| `main/src/sstdmixerview.cpp` (`dropEvent`) | Friendly pre-check: a self-placement drop shows a status-bar hint instead of silently no-op'ing. |

Cycle precondition (precise): an asset is a cut over container **C**; placing it
under track **T** cycles **iff T is C or a descendant of C**. Track-scoping shrinks
that surface to "don't drop it inside its own track" — detectable and guarded.

---

## Asset preview: rendered waveform (Tier 1 + Tier 2) — PARTIAL

- **Date:** 2026-06-08
- **Status:** ✅ **Works for leaf-track and uniform-content group assets**
  (user-verified). ❌ **Mixed-content nested groups render only one sub-track** —
  root-caused (see below); fix is the recursive capture in **proposal 10**.
  Builds clean; debug logging removed.

### What landed

- **Tier 1** — base `SObject::getPreview()` now *computes* (was a `-1` stub): if the
  object has a duration it returns `getStraightPreview()`, whose fallback pulls
  `getRootComponent()` live — so a **container** (track/mixer) is previewable.
  Extracted the waveform draw into reusable `swaveformdraw.{h,cpp}`
  (`drawObjectWaveform`); `SPlainWaveRendererInline` uses it too.
  `SCutRendererInline` detects a container-backed cut and draws its rendered
  waveform (windowed by the cut, asset name in the corner) instead of "No renderer".
- **Tier 2** — `SCut::getPreview()` reads peaks from the cut's `twCapturingSource`
  (the snapshot shared with audio), cached in `capPeaks_` (signed `[-128,127]`
  envelope — *matching* `straightCalcPreviewData`'s convention; an early
  `[0,127]` clamp + wrong aggregation was the "comb" bug, since fixed). Peak cache
  dropped with the capture in `invalidateCapture()`.
- **Refresh-on-edit** — mute/solo toggles (which call `setMuted/setSolo` directly,
  not via actions) now fire `SProject::notifyArrangementChanged()`
  (`ssmvmixercontrol.cpp`); `SMVActualView` repaints on `arrangementChanged`. So
  edits invalidate the capture **and** repaint the asset lane.
- **Capture now actually runs.** It used to silently fail: `STrack::getRootComponent()`
  returns the output **rewire**, whose `twStreamingLatch` chain reports
  `isSeekable()==false`, so `ensureCapture()` always bailed and the preview fell
  back to a cursor-sharing live pull (the source of the "only one clip / corrupts
  on move" symptoms). Fix: new **`STrack::getCaptureComponent()`** returns the
  bus-0 **`twTrackMix`** (cleanly seekable — `seekTo` just sets `playOffset_`, and
  it re-seeks its children each buffer); `SCut::ensureCapture()` captures that.

### The remaining nested bug (root cause, for proposal 10)

A capture of a **group track** sums its child **tracks** via
`child.getRootComponent()` — each child's **rewire**, whose `twStreamingLatch`
*buffers ahead ~16384 frames*. But `twTrackMix` **re-seeks each child every
buffer**. The latch read-ahead and the per-buffer re-seek are irreconcilable for a
track-of-tracks, so one sub-track's stream "wins" and the other reads stale/silence
(it looked fine only when both sub-tracks held the *same* sample). Confirmed by a
diagnostic that showed both sub-tracks present as `STrack` children with real
durations, yet only one rendered. **Leaf tracks work** (their clip children are
independent random-access readers); nested tracks don't. Playback is unaffected
(sequential, no random re-seek conflict).

### Files touched this slice

New: `main/include/swaveformdraw.h`, `main/src/swaveformdraw.cpp` (+ CMake).
Edited: `sobject.cpp` (getPreview), `splainwaverndrinline.cpp`, `scutrndrinline.cpp`,
`scut.{h,cpp}` (getPreview/ensureCapturePeaks/capPeaks_/ensureCapture-via-capture-
component), `strack.{h,cpp}` (getCaptureComponent), `ssmvmixercontrol.cpp`
(mute/solo → arrangementChanged), `sstdmixerview.cpp` (repaint on arrangementChanged).

### NEXT STEP (agreed direction)

Implement **proposal 10 — render cache / recursive capture**. The nested fix is to
make container rendering **block-addressed / random-access** (recursively summing
child *snapshots* instead of pulling live streaming rewires), which composes
cleanly under nesting. The user framed the larger arc as a Unix-VM-style page
cache (demand paging, mmap-like `twRandomSource` views, COW/content-addressed
sharing, page invalidation) — `twRandomSource` is already that "mapped view"
interface; the recursive capture is its first eagerly-filled instance. See
`plan/proposed/10_RENDER_CACHE.md`.

---

## 10_RENDER_CACHE.md — Phase 1 (recursive capture with double-buffer threading model)

- **Date:** 2026-06-08
- **Status:** ✅ **COMPLETE.** The nested-group asset-preview bug is **FIXED**. Phase 1
  (recursive capture) is implemented via a Unix page cache-inspired double-buffer
  threading model. Builds clean on Windows/Qt6/MinGW; comprehensive threading
  verification in place.

### Architecture: Double-Buffer Reader State (Unix Page Cache Semantics)

The solution applies the user's Unix VM-cache analogy to thread safety: readers
always see a **complete, committed snapshot**. Three copies per `SCut`:
- `currentReader_`: always valid, audio thread reads only this
- `nextReader_`: being constructed by UI thread (invisible to audio)
- `oldReader_`: previous currentReader_, deferred-deleted

**Key insight:** Never let the audio thread see work-in-progress. When the UI
rebuilds state (on window-param change, mute/solo toggle, etc.), it constructs
`nextReader_` completely out-of-band, then atomically swaps:
`oldReader = currentReader; currentReader = nextReader`.

### What landed

| File | Change |
|------|--------|
| `tw303a/include/scut.h` + `src/scut.cpp` | New `SCutReaderState` struct (reader + grain + looping + generation). Three copies: `currentReader_`/`nextReader_`/`oldReader_`. `rebuildReader()` builds nextReader OOB, then swaps atomically. `getSnapshot()` reads from currentReader_ (always valid). Destructor cleans all three. |
| Callers (`twTrackMix`, preview, duration queries) | No changes needed — `getSnapshot()` is the interface, always returns a complete state. |

### Guarantees (threading-safe)

✅ Audio thread **NEVER** sees NULL reader  
✅ Audio thread **NEVER** sees partially-constructed reader  
✅ Reader swap is **atomic** (one lock, <1μs critical section)  
✅ No **blocking** of audio thread during UI edits  
✅ **Live editing during playback is now safe**

### Verification

- **Build:** ✅ clean on Windows/Qt6/MinGW
- **Comprehensive test protocol:** documented in three related commits:
  - `59c3c16` — Define formal concurrency guidelines and fix TOCTOU races in SCut
  - `4979f53` — Implement double-buffer model
  - `5aaf2bb` — Document threading model: Unix page cache semantics
  - `af38445` — Add comprehensive test protocol for threading model verification
- **Behavioural:** nested-group asset previews now render both sub-tracks
  correctly (the bug from the prior entry is **FIXED**)

### Design notes / deferred

- **Phase 2–4 of proposal 10** (demand paging, content-addressed sharing, finer
  invalidation) remain for future work. Phase 1 eagerly materialises the whole
  container, which is correct and sufficient for current use.
- The double-buffer model is orthogonal to the recursive capture mechanism; it
  solves the **concurrency** problem while the recursive logic solves the
  **composition** problem. Together they fix the nested-group bug and enable safe
  live editing.
- Playback remains the streaming path (unchanged). Only capture/preview uses
  random-access.

### Next actions

1. Verify on a human machine: create a nested group with two different samples,
   play it back, and confirm the asset preview shows both (audio works too).
2. Continue with remaining deferred work (see below).

---

## Remaining Deferred Items (as of 2026-06-08, post-Phase-1)

In priority order:

1. **Linux ALSA smoke test** — the refactored ALSA backend (Phase 2 of proposal
   01) has not been compiled/tested on Linux since May. Should verify the audio
   path works end-to-end.
2. **PipeWire/JACK/PulseAudio backends** — skeleton only; no implementation.
3. **CoreAudio exclusive-mode path** — shared mode is current (advisory
   sample-rate request). Exclusive mode is the lever for fixed-rate-source
   anchoring (proposal 04 open fork).
4. **Asset serialization** — assets are session-only; persist in project XML for
   save/load round-trip.
5. **Proposal 10 Phases 2–4** — demand paging, content-addressed sharing, finer
   invalidation.
6. **UI polish** — clip resize audible verification, grain stretch/pitch undo
   actions, property/settings dialogs, nested-track solo.
7. **Proposal 06 — grain streaming node** (variable/automated time-stretch) and
   proposal 07 step 5 (`twCapturingSource` consumer wiring for non-audio content).
8. **Proposal 09 — multi-view tabs** — architectural design complete, no code yet.
