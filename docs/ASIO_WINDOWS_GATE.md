# ASIO Phase 1 — the Windows gate run (manual)

> **STATUS: PASSED 2026-08-18** against a real vendor driver (Tascam
> US-16x08). Phase 1 is closed and Phase 2 is unblocked. The record of that
> run — and the driver facts it bought, several of which change Phase 2/4/5 —
> is at the end of this file. The runbook is kept because it should be RE-RUN
> for every further driver: one driver is not a survey, and a wrapper driver
> (FlexASIO / ASIO4ALL) is still untried.

Proposal 35 Phase 1 (PR #31, merged 2026-08-15) put the ASIO SDK detection and
the `asio_probe` spike into the tree. Everything that could be gated by CI-less
automation was (macOS build with the block inert, layering, logging, ctest);
the one thing that could not is the entire point of the phase: **proving on a
real Windows machine that this MinGW-built host can drive an MSVC-built ASIO
driver end to end.** That proof is the exit criterion for Phase 1 — **Phase 2
(the actual backend) starts only after this run reports `GATE PASSED`.**

Why it matters beyond ASIO itself: proposal 21 stopped at L5 because **L6
(duplex latency work) is gated on ASIO/35**, and the recording docs name ASIO
as the fix for the split-clock capture-rate failure class (one driver, one
clock, matched in/out).

## What to do

### 1. Pull main

```bash
git checkout main && git pull
```

### 2. Drop in the Steinberg ASIO SDK

Download the SDK from <https://www.steinberg.net/asiosdk> (free, requires
accepting Steinberg's license). That URL redirects to the current release —
**`ASIO-SDK_2.3.4_2025-10-15.zip`** as of 2026-08-18, ~8.9 MB, the version the
gate run below used. Unzip it and place it so that this exact path exists:

```
smaragd/third_party/asiosdk/common/iasiodrv.h
```

The 2.3.4 zip unpacks to a folder named plainly **`ASIOSDK`** (older 2.3.3
zips used a versioned name) — rename or move it to `asiosdk`. The directory is
**gitignored on purpose**: Steinberg's license forbids redistribution, so it
must never be committed. Only `common/` is ever read: `asio_probe.cc` includes
`iasiodrv.h`, which pulls `asio.h` and `asiosys.h` from beside it, and we
compile ZERO SDK sources.

### 3. Build, and check the configure line

**A plain `./build.sh` on an existing `build/` will NOT pick the SDK up.** The
sentinel is an `EXISTS()` test in `smaragd/tw303a/CMakeLists.txt`, and a
directory appearing is invisible to CMake's dependency graph — so nothing
re-runs configure and `asio_probe` is silently never built. `_env.sh` does
exactly this `touch` for the clap/vst3 submodules for the same reason, but
that path runs from `rebuild.sh`, not from an incremental build:

```bash
touch smaragd/tw303a/CMakeLists.txt   # forces exactly one reconfigure
./build.sh                            # Git Bash, as usual
```

Watch the configure output for:

```
tw_devices: ASIO SDK found — asio_probe spike enabled (proposal 35 Phase 1)
```

If you instead see the `third_party/asiosdk not present` WARNING, the SDK is
in the wrong place (step 2). Worth confirming once while you are here: a build
**without** the SDK (temporarily rename the folder) must also stay green —
the only difference is that WARNING.

### 4. Have at least one driver installed

Best case is a real vendor driver (audio interface). If none is installed,
use **FlexASIO** (<https://github.com/dechamps/FlexASIO/releases>, preferred —
open source, no nag screens) or ASIO4ALL. Running against BOTH a
wrapper driver and a real vendor driver is the ideal outcome; either alone is
still a meaningful gate.

### 5. Run the probe

```bash
./smaragd/build/bin/asio_probe.exe list
./smaragd/build/bin/asio_probe.exe open "<driver name from list>"
./smaragd/build/bin/asio_probe.exe tone "<driver name>" 2
```

- `list` — every driver registered in `HKLM\SOFTWARE\ASIO`, with CLSIDs.
- `open` — loads the driver and reports channels, supported sample rates,
  buffer sizes, latencies, per-channel sample types.
- `tone` — the full lifecycle: you should **hear a 2-second 440 Hz sine**
  (front L/R, quarter scale) and the run should end with
  `GATE PASSED: the MinGW <-> MSVC ASIO ABI works here.`

`<driver name>` is matched case-insensitively as a substring, so
`tone flex 2` works for FlexASIO.

### 6. How to read the output

**Hard failures** (these mean the gate FAILED — stop and report):

- `GATE FAILED` at the end, or any of the `SUSPECT VTABLE MISMATCH` /
  `SUSPECT VTABLE/STRUCT MISMATCH` / `bufferSwitch index outside {0,1}`
  warnings — these are the ABI-mismatch tells the probe exists to catch.
- `delivered far fewer frames than the clock implies` — the stream never ran.
- A crash. (The probe suppresses Windows error dialogs, so a crash is a
  nonzero exit + a short console, not a popup.)

**Not failures — Phase 2 design data.** Note them, don't worry about them:

- `callbacks arrived AFTER stop() returned` — some drivers do this; the Phase
  2 stop-fence is designed for it, and knowing which drivers do is useful.
- `driver sent kAsioResetRequest` — buffer/panel change requests; Phase 2
  handles them as stop-and-reopen.
- `channel reports <type> [UNSUPPORTED TYPE]` for MSB/DSD sample types — the
  backend deliberately supports little-endian int/float only.

### 7. Report the results

Paste the full console output of `list`, `open`, and `tone` (for each driver
tried) into the proposal-35 thread — a comment on PR #31, a note handed to the
next Claude session, or a YouTrack issue if one gets created for Phase 2.
On `GATE PASSED`, Phase 2 (output backend + the `WinMultiBackend` dispatcher —
see `plan/proposed/35_ASIO_BACKEND.md`) is unblocked.

---

## The gate run of 2026-08-18

**Verdict: `GATE PASSED`.** `open` and `tone` both reported it, the 2-second
440 Hz sine was **audible on the connected monitors** (the one thing the probe
cannot check for itself — `RESULT` is a frame count, not a sound), and none of
the ABI tells fired: no `SUSPECT VTABLE MISMATCH`, no `SUSPECT VTABLE/STRUCT
MISMATCH`, no out-of-range `bufferSwitch` index, no callbacks after `stop()`,
no `kAsioResetRequest`, no crash. The MinGW↔MSVC vtable bet — risk 1 of
proposal 35, and the entire reason Phase 1 exists — is settled on real vendor
hardware, exactly as `vst3_probe` settled it for VST3 before M6.

**Setup:** Windows 11, MinGW x64 build from `main` at 585f80a, ASIO SDK
2.3.4, one driver installed: **Tascam US-16x08**, driver version 1001,
CLSID `{FA12DE15-482E-4214-8D11-6817497635C0}`.

### What the driver reported

```
chans  : 16 in, 8 out  (ASE_OK)
buffer : min 256, max 256, preferred 256, granularity 0  (ASE_OK)
rate   : current 44100;  supported: 44100 48000 88200 96000
         all 24 channels Int32LSB
create : 2 ch x 256 frames -> ASE_OK
latency: input 300 frames, output 702 frames  (at 44100)
outputReady: not supported
run    : 48000 Hz, 256 frames/buffer, latency out 735, outputReady no
frames : 91136 delivered, ~96000 expected  (95%)
calls  : 0 bufferSwitch, 356 bufferSwitchTimeInfo
```

### Reading the two lines that look like problems and are not

- **`95%` of expected frames.** Not a dropout. `expected` is computed as
  `rate × seconds` (`asio_probe.cc:628`) against a fixed sleep — it does not
  measure when the stream actually started — so the missing ~4864 frames
  (~101 ms) are the driver's start-up delay inside that window. The probe's
  failure threshold is `delivered < expected / 2`.
- **`0 bufferSwitch, 356 bufferSwitchTimeInfo`.** Not a driver quirk: the
  probe answers `kAsioSupportsTimeInfo` with 1 on purpose
  (`asio_probe.cc:347`) so that a modern driver takes the path the production
  backend will use. The US-16x08 honours it. The obligation this transfers to
  Phase 2 is in the table below.

### What it changes for Phases 2, 4 and 5

One driver is not a survey — but these are the first real numbers this design
has, and three of them change what should be built.

| Measured | Consequence |
|---|---|
| 16 in / 8 out on ONE instance, all `ASIOSTInt32LSB` | Full duplex out of one `AsioDevice` as designed, and 8 outs give proposal 36's wide sink somewhere real to go. The hand-rolled packed **Int24** converter is not exercised by this hardware and stays unit-test-only until a driver that uses it turns up. |
| Rates 44100 / 48000 / 88200 / 96000; **`tone` ran at 48000 while the driver's current rate was 44100** | Native rate selection works. This is the fix for the endpoint sample-rate trap (see CLAUDE.md): one driver, one clock, in and out matched — no 52 144 Hz stream under a 48000 label. `twNegotiator` gets a real `supportedRates()`. No 32k and no 176.4/192k, so the `{32k…192k}` probe sweep correctly returns four. |
| Output latency **702 frames at 44100 but 735 at 48000** | **Latency is RATE-DEPENDENT.** `ASIOGetLatencies` must be read after `setSampleRate` + `createBuffers` and never cached from open. ~1002 frames round trip ≈ 22.7 ms is the figure `SApplication::meterLatencyFrames()` and proposal 21 L6 would work with. |
| buffer min == max == preferred == 256, granularity 0 | Exercises the `granularity == 0 ⇒ {preferred}` branch of the walk. Honest consequence: on this driver the **Phase 4 buffer-size combo has exactly one entry** — the size is set in the vendor's own control panel — so **Phase 5 (the Control Panel button) is worth more than Phase 4 here**, the reverse of the proposal's ordering. Neither is on the critical path; note it when they are scheduled. |
| `outputReady: not supported` | Step 3 of the `bufferSwitch` data path is a no-op on this driver. Keep the call — it is a latency win on many others — but expect nothing from it here. |
| The driver never calls plain `bufferSwitch` once TimeInfo is accepted | **Answer `kAsioSupportsTimeInfo` and you MUST implement `bufferSwitchTimeInfo`.** Implement both entry points into one body, and treat the `ASIOTime` the TimeInfo variant carries as the sample-position source proposal 21 L6 will want. |

### Still not proven

- **No wrapper driver has been run** (FlexASIO, ASIO4ALL). That is the one
  that would exercise the plain-`bufferSwitch` path, a non-zero buffer-size
  granularity, and a driver whose control panel we do not control. Worth doing
  alongside Phase 2 rather than blocking it.
- **No second vendor driver**, so nothing here distinguishes "how ASIO
  behaves" from "how the US-16x08 behaves".
- **Nothing about the stop-fence under stress**: this driver delivered no
  callbacks after `stop()` returned, so the case invariant 3 is being reworded
  for remains unobserved in the wild.
- **No input path at all** — `asio_probe` is output-only; the input half is
  Phase 3.
