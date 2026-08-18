# ASIO Phase 1 — the Windows gate run (manual)

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
accepting Steinberg's license). Unzip it and place it so that this exact path
exists:

```
smaragd/third_party/asiosdk/common/iasiodrv.h
```

The zip usually unpacks to a versioned folder (`asiosdk_2.3.3_...`) — rename
or move that folder to `asiosdk`. The directory is **gitignored on purpose**:
Steinberg's license forbids redistribution, so it must never be committed.

### 3. Build, and check the configure line

```bash
./build.sh          # Git Bash, as usual
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
