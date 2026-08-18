# Proposal 35 — ASIO audio backend (Windows)

**Status:** **Phase 1 CLOSED 2026-08-18.** It landed 2026-08-15 (PR #31 —
SDK detection + `asio_probe`) and its exit criterion — the manual Windows
gate run — **PASSED on that date against a real vendor driver** (Tascam
US-16x08, ASIO driver version 1001, on the MinGW x64 build): `open` and
`tone` both reported `GATE PASSED`, with an audible 440 Hz sine on the
connected monitors and no ABI tell of any kind. **Phase 2 is unblocked** —
but see the proposal-36 note below: it must be re-planned before it is
written. The run, and the driver facts it bought, are recorded in
`docs/ASIO_WINDOWS_GATE.md` § "The gate run of 2026-08-18". Phases 2–5 not
started.

**Post-landing notes (2026-08-18):**

- **Proposal 36 (multichannel signal flow) executed 2026-08-16, AFTER this
  design was written**, so the Phase 2 output path had to be re-planned
  against 36's channel model. **That was done on 2026-08-18 — see
  "Phase 2, re-planned" below, which is now the authority.** Two corrections
  it makes to this note as originally written, kept here because the wrong
  reading is the tempting one: the `c % 2` fan-out named here is
  `twmonitor::interleave` in `twSpeaker`, **not** anything in this proposal,
  and it is current shipped behaviour that Phase 2 does not touch; and
  `RenderCallback` is still interleaved, so the output half's data path
  description needed no edit at all. What genuinely changed is the
  `AudioConfig::channels` question — how many of a pro interface's outputs to
  open — which WASAPI shared mode never posed. The dispatcher/id scheme, the
  registry/facade split, the input ring and the SDK-free loading strategy are
  unaffected.
- **Proposal 21 stopped at L6 explicitly gated on this proposal** (duplex
  latency work needs one driver/one clock), and the recording docs name ASIO
  as the fix for the split-clock capture-rate failure class — this proposal
  is now on the critical path of two others.
- **What the gate run measured (2026-08-18, Tascam US-16x08).** One driver is
  not a survey, but these are the first real numbers this design has and three
  of them change what Phase 2/4/5 should do. Full output and reasoning in
  `docs/ASIO_WINDOWS_GATE.md`.

  | Measured | Consequence for this design |
  |---|---|
  | 16 in / 8 out on ONE instance; all channels `ASIOSTInt32LSB` | Full duplex out of one `AsioDevice` as designed, and 8 outs give 36's wide sink somewhere real to go. The hand-rolled packed Int24 converter is NOT exercised by this hardware and stays unit-test-only. |
  | Rates 44100 / 48000 / 88200 / 96000; the run opened at 48000 while the driver's current rate was 44100 | Native rate selection works, which is the whole point: `twNegotiator` gets a real `supportedRates()` and the split-clock capture-rate failure class has one clock. No 32k and no 176.4/192k — the `{32k…192k}` sweep correctly returns four. |
  | `ASIOGetLatencies` gave out 702 at 44100 and out 735 at 48000 | **Latency is RATE-DEPENDENT.** It must be read after `setSampleRate` + `createBuffers`, never cached from open. ~1002 frames round trip ≈ 22.7 ms is the number `meterLatencyFrames()` and 21 L6 would work with. |
  | buffer min == max == preferred == 256, granularity 0 | The `granularity == 0 ⇒ {preferred}` branch. On this driver the Phase 4 buffer-size combo has EXACTLY ONE entry — the size is set in the vendor's own control panel — so **Phase 5 is worth more than Phase 4 here**, the reverse of the order below. Neither is on the critical path; note it when they are scheduled. |
  | `outputReady: not supported` | Step 3 of the data path is a no-op on this driver. Keep the call (a win on many others), expect nothing from it here. |
  | 0 `bufferSwitch`, 356 `bufferSwitchTimeInfo` | Not a driver quirk — the probe answers `kAsioSupportsTimeInfo` with 1 on purpose. It does mean the production backend inherits the obligation: **answer that message and you MUST implement `bufferSwitchTimeInfo`**, because a driver that honours it will never call plain `bufferSwitch` again. |

## Why

Smaragd's Windows audio is WASAPI shared mode only — no exclusive/low-latency
path, and the buffer-size combo in the Options dialog is dead there (only ALSA
implements `getAvailableBufferSizes`). ASIO is the standard low-latency driver
model on Windows and unlocks: user-controllable buffer sizes, real multi-rate
negotiation (`twNegotiator` can pick the project rate natively instead of
resampling to the shared-mode mix rate), sample-accurate latency reporting for
the level meters (proposal 34), and pro-interface I/O.

Decisions taken with the requester (2026-08-11):

- **Full duplex.** ASIO serves both playback (`audio::AudioBackend`) and
  recording (`audio::AudioInput`) from one shared driver instance.
- **Unified device list.** WASAPI devices and ASIO drivers appear in ONE list,
  ids namespaced `wasapi:<endpoint-id>` / `asio:<clsid>`. The device-picker
  menu and the Options combos work unchanged — an ASIO driver is just another
  entry.
- **SDK is a manual drop-in.** Steinberg's ASIO SDK license forbids
  redistribution, so unlike clap/vst3_pluginterfaces it cannot be a submodule.
  The user unzips it to `smaragd/third_party/asiosdk`; CMake detects the
  sentinel `common/iasiodrv.h` (the VST3 block's pattern) and the directory is
  gitignored. Absent SDK ⇒ clean build, ASIO disabled, one configure WARNING.

## Architecture

One shared **`AsioDevice`** core owns the `IASIO` COM instance, the
double-buffer allocation for input+output channels, the `bufferSwitch`
callback, and an SPSC ring for input. Two thin facades sit on top —
**`AsioBackend : audio::AudioBackend`** and **`AsioInput : audio::AudioInput`**
— obtained through a process-wide **`AsioDeviceRegistry`** (weak_ptr cache
keyed by CLSID). At most one live ASIO driver per process: ASIO callbacks
carry NO context pointer, so a process-global active-device pointer is
mandatory anyway; the registry makes the restriction explicit and gives the
second caller a readable error instead of undefined driver behaviour. Driver
start/stop is REFCOUNTED across the two facades, so starting a recording
while ASIO playback runs does not restart the driver, and recording alone
starts it with the output half emitting silence.

On Windows, `createAudioBackend()` / `createAudioInput()` return dispatchers
(**`WinMultiBackend`** / **`WinMultiInput`**) that merge both device lists and
route by id prefix. **Bare/legacy ids route to WASAPI** — backward compatible
with every persisted `audio/deviceId`, and the per-device latency keys
(`audio/outputLatency/<id>`) keep working because the full prefixed string is
the key. Mixed mode (WASAPI out + ASIO in, or vice versa) works by
construction; ASIO-driver-A out + ASIO-driver-B in is rejected by the registry
with a clear `errorMessage()`.

### The MinGW ABI decision (what Phase 1 exists to prove)

Compile **zero SDK sources.** Use only the SDK headers (`common/iasiodrv.h`,
`common/asio.h`) for the types, CoCreate the driver ourselves, and call the
`IASIO` virtuals directly on the returned object. This sidesteps the MSVC-isms
in `host/pc/asiolist.cpp` / `host/asiodrivers.cpp` entirely — driver discovery
is our own ~100-line scan of `HKLM\SOFTWARE\ASIO` (`asio_driver_list.cc`,
pure advapi32, no SDK dependency). On x64 there is a single calling
convention, so MinGW-w64 g++ calling an MSVC-built C++ vtable works — the
same bet `vst3_probe` proved for VST3 before M6 was written. x86 is NOT
supported (`#error` under `!_WIN64`); the shipped build is x64-only anyway.

ASIO's COM usage is idiosyncratic and the probe must exercise all of it:
`CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, …)` — the
driver's CLSID doubles as its IID; `init(sysRef)` takes an HWND; the four
callbacks (`bufferSwitch`, `bufferSwitchTimeInfo`, `sampleRateDidChange`,
`asioMessage`) are plain C function pointers invoked from a driver-owned
thread.

### `bufferSwitch(index)` data path (driver thread, RT rules)

Both entry points land in the same body. Answering `kAsioSupportsTimeInfo`
makes a modern driver call `bufferSwitchTimeInfo` and **never call plain
`bufferSwitch` again** (measured: 356 and 0 on the US-16x08), so implementing
one of the two is not optional — implement both, and treat the `ASIOTime` the
TimeInfo variant carries as the sample-position source 21 L6 will want.

No locks, no allocation, no logging — scratch buffers are pre-allocated at
`createBuffers` time; errors latch into atomics and are logged from the
control plane.

1. **Output half:** if a render callback is attached, call it into the
   interleaved float scratch (`RenderCallback(out, bufferFrames, outCh)`),
   zero any shortfall, then per output channel convert interleaved float →
   that channel's `ASIOSampleType` into `bufferInfos_[ch].buffers[index]`
   (strided read, contiguous write). Not attached ⇒ memset silence (covers
   input-only recording and a driver that starts calling before the app is
   ready).
2. **Input half:** if enabled, per input channel convert native → interleaved
   float scratch, push into the SPSC ring (drop-oldest on overflow, drops
   counted and logged from outside the callback).
3. `ASIOOutputReady()` if the driver supports it (latency win on many).

Sample types supported: `ASIOSTInt16LSB`, `ASIOSTInt24LSB` (packed 3-byte,
hand-rolled — `twSampleType` has no Int24 and `twConvertFrames` is
interleaved-only, so the converters live in `devices/src/asio_convert.h`,
pure C++ with no SDK types ⇒ unit-testable on macOS), `ASIOSTInt32LSB`,
`ASIOSTFloat32LSB`, `ASIOSTFloat64LSB`. MSB (big-endian) and DSD types are
rejected at open with a clear error.

`stopOutput()` semantics: the callback thread is DRIVER-owned, so there is no
thread to join; the invariant callers rely on ("no callback in flight or
forthcoming after return") is preserved by an atomic callback-depth fence
spun after `ASIOStop()` returns. CONTRACT invariant 3 gets reworded
accordingly. `kAsioResetRequest` / `sampleRateDidChange`: latch, log via
TW_LOG, stop; the device reopens on the next Play (declared known debt — no
live renegotiation).

## Files

| File | Contents |
|---|---|
| `tw303a/devices/src/asio_driver_list.{h,cc}` | Registry scan → `{name, clsid, description}`. Pure advapi32/ole32, no SDK dep. (Phase 1) |
| `tw303a/devices/tools/asio_probe.cc` | Spike/triage tool, the ABI gate — analog of `plugins/tools/vst3_probe.cc`. (Phase 1) |
| `tw303a/devices/src/asio_convert.h` | De-interleaved float↔Int16/24/32/Float32/64 converters, SDK-type-free, cross-platform testable. (Phase 2) |
| `tw303a/devices/src/asio_device.{h,cc}` | `AsioDevice` + `AsioDeviceRegistry`; trampolines + `s_active`; refcounted start/stop; buffer-size walk (`ASIOGetBufferSize` granularity); rate probe (`ASIOCanSampleRate` over {32k…192k}). (Phase 2/3) |
| `tw303a/devices/src/asio_backend.{h,cc}` | `AsioBackend : AudioBackend` facade. Private header, like `wasapi_input.h`. (Phase 2) |
| `tw303a/devices/src/asio_input.{h,cc}` | `AsioInput : AudioInput` facade; `read()` drains the ring with 1 ms sleeps + timeout (RecordingSession already loops + resamples). (Phase 3) |
| `tw303a/devices/src/spsc_ring.h` | Header-only SPSC float ring (power-of-two, acquire/release). (Phase 2, used Phase 3) |
| `tw303a/devices/src/asio_id.h` | `parseDeviceId(id) → {Wasapi\|Asio\|Default, nativeId}` — pure routing, shared by both dispatchers, cross-platform testable. (Phase 2) |
| `tw303a/devices/src/win_multi_backend.{h,cc}` | Output dispatcher. Compiled whenever `QBX_WIN_WASAPI` (ASIO halves under `#ifdef TW_HAVE_ASIO`) so prefixed ids never break in SDK-less builds. (Phase 2) |
| `tw303a/devices/src/win_multi_input.{h,cc}` | Input dispatcher. (Phase 3) |
| `tw303a/devices/tests/multi_backend_test.cc` | Cross-platform ctest: id routing, SPSC ring, granularity-walk math, converters. (Phase 2) |

Modified: `devices/src/audio_backend.cc` + `audio_input.cc` (Windows branch
returns dispatchers; other platforms untouched), `smaragd/CMakeLists.txt`
(`option(ENABLE_ASIO)`), `tw303a/CMakeLists.txt` (sentinel block),
`.gitignore` (SDK), `devices/CONTRACT.md` (invariants 3/4/5 + new
one-driver-per-process invariant), `main/servicesui/src/soptionsdialog.cpp`
(Phase 4/5). Phase 5 only: one default-implemented
`virtual int openControlPanel() { return -1; }` on `AudioBackend`.

Reused unchanged: `twSpeaker` (backend built once in ctor; the dispatcher
makes WASAPI↔ASIO switching a plain device-id change, honoring the existing
"takes effect on next Play" model), `twNegotiator` (consumes the real
`supportedRates()`), `SApplication::meterLatencyFrames()` (`ASIOGetLatencies`
returns frames; existing device→project scaling applies),
`RecordingSession` (already loops `read()`, resamples, CoInitializes its
worker — fine for CoCreating the driver there; drivers are conventionally
called cross-thread without marshaling).

## Phases (one PR each)

1. **SDK detection + `asio_probe`** — CMake option + sentinel block (probe
   only), `.gitignore`, `asio_driver_list`, the probe (`list` / `open` /
   `tone` subcommands). *Gated:* build green on macOS (block inert) and
   Windows with AND without the SDK; layering/logging/ctest unchanged.
   *Ungated:* probe run against FlexASIO / ASIO4ALL and one real driver —
   Windows-manual. **DONE 2026-08-18: `GATE PASSED` on a real vendor driver
   (Tascam US-16x08), tone audible — on BOTH callback entry points.** The
   legacy path needed no second driver: `--no-timeinfo` declines
   `kAsioSupportsTimeInfo` and the driver falls back (measured 357/0 against
   the default's 0/322), so it honours the negotiation in both directions. A
   wrapper driver (FlexASIO / ASIO4ALL) has still NOT been run; what it would
   add is a **non-zero buffer granularity**, which this driver cannot produce
   (min == max == preferred == 256) and which is unit-tested rather than
   hardware-gated. Optional, never blocking.
2. **Output + dispatcher** — `asio_convert`, `asio_bufsize`, `asio_device`
   (output half + registry + gate/fence), `asio_backend`, `asio_id`,
   `win_multi_backend`, factory change, CONTRACT edits,
   `multi_backend_test`, `audio_backend_probe`. *Ungated:* playback via
   FlexASIO; meter latency sanity against a real measurement;
   `setBufferSize` on a driver that offers more than one size.
   **RE-PLANNED 2026-08-18 against proposal 36 and the gate run — read the
   section below before writing any of it. EXECUTED 2026-08-18**, with the
   real-driver checks done through `audio_backend_probe` (ASIO open/tone,
   and the WASAPI regression at all three id spellings).
3. **Input + full duplex** — input half of `bufferSwitch`, `asio_input`,
   `win_multi_input`, factory change, refcounted cross-facade start/stop.
   *Ungated:* ASIO record-only; record-while-playing on the same driver;
   both mixed modes; A-out/B-in rejected cleanly.
4. **Options input-device enumeration** — replace the hardcoded "System
   default" input combo with `createAudioInput()->listDevices()` (closes the
   long-standing Phase-7 TODO). ASIO entries report channels=0 in enumeration
   so listing does not load every driver.
5. **(optional) Control Panel button** — `openControlPanel()` default virtual,
   `ASIOControlPanel()` behind it, Options button enabled for `asio:` ids;
   panel-driven buffer changes surface as `kAsioResetRequest` → reopen on
   next Play.

## Phase 2, re-planned (2026-08-18)

The header note says the Phase 2 output path is stale after proposal 36. It
is — but **narrower, and in a different place, than that note claims**, and
the difference is worth stating because the obvious re-reading throws away
work that is still correct. This section supersedes the Phase 2 bullet above
where the two disagree.

### What is NOT stale, verified against the tree

- **`RenderCallback` is unchanged**: `std::function<size_t(float *out,
  size_t frames, uint32_t channels)>`, **interleaved**
  (`devices/include/tw/devices/audio_backend.h:46`). Proposal 36 went planar
  at `AudioEngine::pullBlock`, which is one seam ABOVE the backend. So the
  output-half description in "§ `bufferSwitch(index)` data path" — pull
  interleaved float into scratch, zero the shortfall, convert per channel
  into `bufferInfos_[ch].buffers[index]` — is **still exactly right** and
  needs no edit.
- **The `c % 2` fan-out the header calls stale is not in this proposal at
  all.** It is `twmonitor::interleave` (`playback/include/tw/playback/
  twspeaker.h:57`), it is CURRENT shipped behaviour, and it is what proposal
  36 B5 deliberately left in place: `L = ch0; R = (width >= 2) ? ch1 : ch0`,
  that pair then meeting the device's channel count. Nothing in Phase 2 has
  to touch it.
- **Nothing outside a backend reads an output `AudioConfig::channels`.**
  Checked repo-wide: the only consumers are the backend's own conversion and
  the `channels` argument `twSpeaker`'s callback receives. `RenderSession`,
  the file writers and `CaptureBridge` all carry their own, unrelated
  `channels`.
- The dispatcher/id scheme, the registry/facade split, the input ring and the
  SDK-free loading strategy are unaffected, as the header already says.

So "Reused unchanged: `twSpeaker`" — a line that was in doubt — **stays
true**, and that is a decision, not an accident. See the next part.

### The one thing proposal 36 really does change: how many outputs to open

WASAPI shared mode handed this design a 2-channel endpoint and the question
never arose. ASIO does not: the gate driver has **8 outputs**. Because
`twmonitor::interleave` writes `out[i * deviceChannels + c] = (c % 2 == 0) ?
l : r`, a backend that reports `channels = 8` puts the monitor mix on OUT 1/2
**and** 3/4 **and** 5/6 **and** 7/8 — which on a pro interface are routinely
headphone amps and outboard sends.

> **DECIDED (requester, 2026-08-18): the ASIO device opens OUTPUTS 1–2 ONLY.**
> `createBuffers` is called for output channels 0 and 1 (fewer if the driver
> has fewer), and `getConfig().channels` reports that number — never the
> driver's `ASIOGetChannels` output count.

Why this and not the alternatives:

- It is **exactly the shipped monitoring rule**, not an approximation of it.
  Proposal 36 §8 names channel roles and a fold law as non-goals, and the
  device rule that came out of that is "monitoring is stereo, rendering is
  not". An ASIO backend that opened eight outputs would be the first thing in
  the tree to have an opinion about physical output routing, and it would be
  expressing that opinion through a `c % 2` accident rather than a design.
- It makes **`twSpeaker` need zero changes for ASIO output**, so the
  WASAPI-through-dispatcher regression and the ASIO path exercise the same
  code above the backend — which is what makes that regression meaningful.
- It is the cheapest correct thing: no conversion of six channels of silence
  every block, on the driver thread, under RT rules.

What it forgoes, stated plainly so nobody discovers it as a bug: **a
6-channel project is monitored on OUT 1/2 and its other channels are not
reachable from any physical output**, and monitoring cannot be routed to,
say, OUT 3/4. Both need an output-routing model (which physical output is
"main", what a wider project does with the rest). That is its own proposal,
after Phase 2 lands, and it would revisit proposal 36's device rule rather
than extend this one. A render is unaffected — it already writes the
project's full width to a file.

### What the gate run adds to the Phase 2 spec

From `docs/ASIO_WINDOWS_GATE.md` § "The gate run of 2026-08-18". These are
requirements, not observations:

1. **Implement BOTH callback entry points into one body.** We answer
   `kAsioSupportsTimeInfo`, and a driver that honours it **never calls plain
   `bufferSwitch` again** (measured 356 / 0). Implementing only the one the
   design's prose names would produce a silent, non-obvious dead stream on
   every modern driver. Keep the `ASIOTime` the TimeInfo variant carries: it
   is the sample-position source proposal 21 L6 will want, and it is free
   here.
2. **Read `ASIOGetLatencies` AFTER `setSampleRate` + `createBuffers`, and
   again after any rate change.** Measured 702 frames out at 44100 and 735 at
   48000 on one driver — latency is rate-dependent, so an open-time cache is
   wrong by 33 frames before anything interesting happens. It feeds
   `AudioConfig::outputLatencyFrames`, hence `meterLatencyFrames()`, hence
   every position the meters, MIDI-out pump and both recorders compensate
   with.
3. **`ASIOSTInt32LSB` is the type that matters first** — it is what the gate
   driver uses on all 24 channels. The full set stays as designed; the packed
   **Int24** path is the one with no hardware behind it, so it is
   unit-test-only until a driver turns up that needs it. Say so in the PR
   rather than implying it was exercised.
4. **`outputReady()` may be unsupported** (it is, on the gate driver). Call
   it when the driver advertises it, treat its absence as normal, and never
   let the return value gate anything.
5. **The `granularity == 0 ⇒ {preferred}` branch of the buffer-size walk is
   the real-hardware case**, not a corner: the gate driver reports
   min == max == preferred == 256. The consequence for Phase 4 is recorded in
   the header table — the combo would have one entry — and it is why Phase 5
   is worth more than Phase 4 on that hardware.
6. **`supportedRates()` comes from `ASIOCanSampleRate` over the sweep** and
   really is a short list (44100 / 48000 / 88200 / 96000 on the gate driver,
   with no 32k and no 176.4/192k). `twNegotiator` can then pick the project
   rate natively — which is the whole point, and the fix for the endpoint
   sample-rate trap in CLAUDE.md.

### Phase 2 deliverables, revised

Unchanged from the table above: `asio_convert.h`, `asio_id.h`,
`asio_backend.{h,cc}`, `win_multi_backend.{h,cc}`, the factory change, the
CONTRACT edits, `multi_backend_test.cc`.

Changed:

- `asio_device.{h,cc}` — add the two-output policy (`outs = min(2,
  deviceOutCount)`, buffers created for exactly those), both callback
  trampolines into one body, and the latency re-read ordering.
- `asio_bufsize.h` — NEW, and not in the table above. The granularity walk was
  listed as living inside `asio_device`; it is a free function in its own
  SDK-free header instead, for the same reason `asio_id` and `asio_convert`
  are: it is the part that can be gated without hardware, and the gate driver
  exercises only one of its three branches.
- **`spsc_ring.h` is CANCELLED.** `tw/devices/audio_ring.h` is already a
  lock-free SPSC ring — head/tail atomics, no mutex — and is already driven by
  the WASAPI, ALSA and file capture threads, which is the same shape an ASIO
  `bufferSwitch` has. Phase 3 uses `AudioRing`. One was written before this was
  checked, and it had reintroduced precisely the bug `devices/CONTRACT.md`
  inv. 20 already forbids (drop-OLDEST on overflow needs the producer to move
  the consumer's index, which is a data race); the threaded test caught it
  reordering. Two implementations of a lock-free structure is the duplication
  this repo has paid for before.
- `audio_backend_probe.cc` — NEW tool, not in the table above and the reason
  three real bugs were found rather than shipped. It drives the PRODUCTION
  path (`createAudioBackend` → dispatcher → backend) with `list` / `open` /
  `tone`, and it is the ONLY way any of Phase 2 can be exercised against a
  driver; `asio_probe` proves the ABI but bypasses every class Phase 2 adds.
- `multi_backend_test.cc` — the granularity walk covers the `granularity == 0`
  case with `min == max == preferred` as a named row, and the converters cover
  de-interleave per type plus packed Int24, saturation, and the deliberate
  non-clamping of the float types.
- Nothing in `tw/playback`. If a Phase 2 diff touches `twSpeaker`, the
  two-output decision above has been dropped somewhere and that is the thing
  to re-examine first. (It did not: the diff is `tw/devices` and CMake only.)

### What Phase 2 cost that nothing predicted

Three bugs, none of which any amount of reading would have found, and all
three from the hardware probe rather than the suite:

1. **`ASIOCallbacks` MUST OUTLIVE `createBuffers`.** The driver does not copy
   the struct — it keeps the POINTER and dereferences it on every callback for
   the life of the stream. A local in `createBuffers_()` therefore dies while
   the driver is still calling through it: an immediate SIGSEGV **on the
   driver's thread, at a stack address**, with a backtrace of pure garbage
   because the "return address" never was one. `asio_probe` is accidentally
   immune (its struct is a local in `cmdTone`, which spans the whole run), so
   the ABI gate could never have surfaced it. Now a member.
2. **The stop fence needed a GATE, not just a wait.** `stopOutput()`'s contract
   is "no callback in flight OR FORTHCOMING", and on a driver-owned thread the
   second half cannot be delivered by waiting — nothing stops the driver
   entering the trampoline once more after `ASIOStop()` returns, and the gate
   driver does exactly that, **256 frames, on every run**. Measured: the
   in-flight spin alone let a late callback through into the app's render
   callback, which is precisely the teardown hazard the invariant exists for.
   `acceptCallbacks_` is cleared BEFORE `ASIOStop`, so a late callback is
   turned away having touched nothing, and is counted and logged rather than
   hidden. Note this contradicts the Phase 1 gate run, which reported no late
   callbacks — the probe stops differently, and one observation of "this driver
   does not" was not evidence.
3. **Enumeration must NOT prefix WASAPI ids.** The design says ids are
   namespaced `wasapi:` / `asio:`; emitting the `wasapi:` half would have been
   a silent regression for every existing user, because the device picker
   compares its menu entries against the current device string VERBATIM
   (`smainwindow.cpp`) — a stored bare id would match nothing, the menu would
   check-mark the FIRST entry, and applying the Options page would then switch
   the user's device. The prefix is accepted on input and never produced.
   (Relatedly, the design's claim that "the per-device latency keys
   `audio/outputLatency/<id>` keep working" is moot: no such key exists. The
   only per-device settings key is `audio/recordingOffsetMs/<input NAME>`,
   which the output id does not touch.)

## Risks

1. **MinGW↔MSVC vtable ABI** — Phase-1 probe gates it before any backend
   lands; x64-only `#error`; VST3 precedent in-repo.
2. **Badly behaved drivers** (blocking `init`, message-pump needs, crash on
   `Release`) — probe doubles as the triage tool; control-plane calls run on
   the Qt main thread (has a pump) or the CoInitialized recording worker;
   FlexASIO is the reference test driver.
3. **Buffer-size change while the shared device records** — refused unless
   the start-refcount is 0; Options already guards on `!isPlaying()`.
4. **Rate change / reset mid-stream** — latch + log + stop; known debt.
5. **SDK licensing** — gitignored, never committed; the configure WARNING
   carries the drop-in instructions.

## Test strategy

- **CI-gated (ctest, all platforms):** id parse/route table, SPSC ring
  (wraparound, overflow-drop, threaded smoke), buffer-size granularity walk
  (−1 ⇒ pow2 min..max, 0 ⇒ {preferred}, >0 ⇒ arithmetic steps capped ~32),
  converters incl. de-interleave and Int24 packing.
- **Windows-manual, named as ungated in every PR body:** everything touching
  a real driver — enumeration, open/start/stop, duplex, latency numbers,
  control panel, and the WASAPI-through-dispatcher regression.
