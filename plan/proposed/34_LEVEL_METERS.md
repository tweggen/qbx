# Proposal 34 — Level meters (per-track + master)

> **Status: EXECUTED (2026-08-05).** M0..M5 landed together. Remaining work is
> coverage and polish, not capability — see "Deliberately not done".

Prerequisite reading: `docs/contracts/THREADING.md` (rule 1, and the metering
paragraph added by this proposal), `smaragd/tw303a/metering/CONTRACT.md`,
`smaragd/main/timeline/CONTRACT.md` (inv. 10-12),
`plan/proposed/16_STALE_PAGE_FALLBACK.md`, `plan/proposed/19_ASYNC_FREEZE_MODEL.md`.

## Why

Smaragd had faders, mute/solo, plugin inserts and a render path, but **no visual
level feedback anywhere** — no way to see which track contributes what, to catch
a clipping insert, or to confirm a muted track is silent. The intent was on
record twice and deferred twice (`docs/UNIFIED_RENDERING_ARCHITECTURE.md:648`,
`plan/06_RECORDING_MVP.md:23,305`), and a dead stub had been sitting in
`SApplication` since the beginning: `setSpeakerMaxVal()`, body
`// FIXME: insert for VU.`, zero callers.

## The headline finding: no engine changes were needed

Frozen pages are **position-keyed**. A page frozen four seconds ahead of the
playhead still *describes* the audio at the position it covers, so "scan the page
covering the (latency-compensated) playhead" is inherently "what is audible" — no
matter which worker thread froze it or how far ahead.

That kills the obvious design. Computing a peak inside `freezePage` and storing it
in a per-track atomic is **wrong by construction**: pages are frozen by
readahead/revalidator workers far ahead of the playhead (65536 frames ≈ 1.37 s at
48 kHz) and by renders with no playhead at all, so the meter would show the
future and jump erratically. Reading pages by position instead meant the whole
feature is a new engine *leaf* module plus app code, with **zero edits to any
existing engine file** — so the render byte-`cmp` exactness gate is green by
construction.

## What the exploration established

1. **The tap is the per-track `twRewire`, and nothing else works.**
   `twTrackMix::freezePage` (`tw303a/mix/src/twtrackmix.cc:346`) allocates a fresh
   `twOutputPage` on *every* call and never populates `outputPages_`;
   `twPluginChain::freezePage` (`tw303a/plugins/src/twpluginchain.cc:214`) renders
   nothing and forwards to `requestPage()` on its last insert. Neither ever
   answers `getPageIfExists()`. `twRewire` has no `freezePage` override, so
   `STrack::getRootComponent()` (`strack.cpp:97-100` → `cpRewire_`) goes through
   base `twComponent::freezePage`, which caches and stamps `contentEpoch`. Its
   pages are frozen at page-aligned playback positions on both paths — the legacy
   pull (`twStreamingLatch::copyData`, `twstreaminglatch.cc:100-107`) and the
   scheduler (base `planPage` walks input plugs, `twcomponent.cc:751-757`).
   Content there is post-fader, post-FX, pre-summing, and is exactly what the
   master sums at unity (`sstdmixer.cpp:190-193`).
   *Consequence:* "post-fader" and "post-FX" are the same tap, and a pre-fader
   option cannot be offered without new engine work. Say so; don't pretend it's a
   preference.

2. **Latency compensation was already fully supported.**
   `AudioConfig::outputLatencyFrames` exists and *every* backend populates it
   (CoreAudio `coreaudio_backend.cc:237`, WASAPI `:288`, ALSA `:221`, Null `:18`),
   `AudioBackend::getLatencyFrames()` is the accessor, and
   `twSpeaker::getBackend()` was already public and already used by the app.
   **The trap:** it is in DEVICE frames at the device rate while the locator counts
   PROJECT frames — it must be scaled by `projectRate / deviceRate`, or a
   44.1 kHz project on a 48 kHz device over-compensates by ~9%.

3. **Leave `twAspectMetadata` alone.** `twComponent::freezePage` already stores
   `validAspects = twAspectAll` unconditionally (`twcomponent.cc:622`), so the
   "peak levels" bit is already set and already means nothing. Giving it meaning
   would drag metering into the demand/revalidation system for no benefit over
   reading pages that already exist. Neither enum was touched.

4. **Master reads the graph, not the device** (requester's call). The master probe
   runs against the very component the engine plays (`SApplication::rootComponent()`)
   by the same mechanism as the tracks, so master and tracks are mutually
   consistent and `twSpeaker`/`AudioEngine`/the RT callback are **not touched at
   all**. Accepted consequence: an underrun reads as normal level, not as a dip.

5. **Mono.** `STrack` does `setNBusses(2)` but `SStdMixer` runs one bus
   (`sstdmixer.cpp:433`) and `freezePage_nolock` renders `idx = 0` only, so there
   is no second channel to meter. Never write an `L != R` assertion.

## Design

- **`twLevelProbe`** (`tw/metering`) reads a tap's pages by position. Window is
  `[max(lastPos, pos-MAX_WINDOW), pos)`; `MIN_WINDOW`=256 when the position did
  not advance, `MAX_WINDOW`=4800 (100 ms) when the pump was starved — capping the
  work while still measuring the frames about to be heard. Clamped into one page
  and into `[0, validFrames)`. **Never blocks, waits or demands**: it reads via
  `getPageIfExists` (try-lock) and a miss is the caller's cue to decay.
- **Acceptance ladder**, a read-only echo of `AudioEngine::updateFrozenPage`:
  frozen+current → frozen-but-**stale accepted anyway** (proposal-16 parity: that
  is what playback serves) → the placeholder's `stalePredecessor` → last tick's
  held page (this is what makes a lost try-lock survivable) → miss.
- **`twMeterBallistics`** runs on the UI thread, driven by wall-clock dt, not tick
  count: peak decay 20 dB/s dB-linear, hold 1.5 s then 12 dB/s, RMS one-pole with
  `alpha = 1 - exp(-dt/0.3)`. **Frame-rate independence is the load-bearing
  property** and is asserted (one 1 s step == 100 × 10 ms steps). An engine-side
  accumulator could not have offered it.
- **One pump, one position.** `SApplication::pumpMeters` (`meterTimer_`, 33 ms,
  self-stopping after a ~8 s decay tail) subtracts the latency ONCE and emits
  `meterTick(pos, nowMs, live)`, so every meter agrees. Separate from
  `pumpLocator`, which only works when the position *changed* and stops the
  instant playback stops — meters need a tick at a static position and a tail, or
  the bars freeze mid-level and read as a rendering bug. Not started during an
  offline render (which publishes positions faster than realtime); started while
  recording (monitoring playback is live).
- **`SLevelMeter`** paints peak + held tick + inner RMS bar + latching clip cap on
  a −60…+6 dBFS dB-linear scale, and repaints only when the pixel-quantized
  drawing changes (hidden ⇒ zero work; sub-rect `update()`; what was *painted* is
  the ground truth). Mounted beside the fader in the track head, in the Track
  Detail dock, and horizontally in the transport toolbar.

## Semantics decided

| Question | Decision |
|---|---|
| Tap | Track root `twRewire`: post-fader, post-FX, pre-summing. Master: the mixer root the engine plays. |
| Pre-fader | Not offered — `twTrackMix`'s page is not cached. |
| Mute / solo | Followed, by an EXPLICIT model check (`!muted && (!anySolo \|\| solo)`) as well as the emergent nulled-plug behaviour, so it is order-independent (THREADING rule 4). A muted track's own pages can legitimately still hold audio. |
| Scale / zones | −60…+6 dBFS, dB-linear. Green < −9, amber −9…−1, red > −1. |
| Clip | Latches at 0.999 (a float that round-tripped through 16-bit PCM lands on 32767/32768). Cleared by clicking, or on transport start. Never time-based. |
| Stopped | Decay to the floor, then the pump self-stops. |
| Latency | Compensated. Meters therefore trail the DRAWN playhead by the device latency, since the playhead is itself uncompensated — documented, not hidden. |

## What landed

| Milestone | Module | Content |
|---|---|---|
| M0 | new `tw/metering` | `tw_level_scan.h`, `tw_meter_ballistics.{h,cc}`, `tw_level_probe.{h,cc}`, `metering_test`, CONTRACT. No existing engine file touched. |
| M1 | `app/shell` | `meterTimer_`/`pumpMeters`/`meterTick`/`meterReset`, `meterLatencyFrames()` with the device→project rate scaling. Deleted the dead `setSpeakerMaxVal`. |
| M2 | `app/timeline` | `SLevelMeter`; mounted in `SSMVMixerControl` with density rules (Full → vertical beside the fader; Compact → horizontal, only when ≥ 60 px; Tiny → hidden). |
| M3 | `app/shell` | `masterProbe_`/`masterLevel()` + the transport-bar meter. |
| M4 | `app/timeline` | Track Detail dock meter, **plus** the fix below. |
| M5 | `app/testkit` | `assert-meter`, `meter_levels.qxa`, `meter_postfader.qxa`, `SMainWindow::describeTrackMeter` / `grabLevelMeter`. |

Width budget (verified, M2): at the 120 px default column the content width is
92 px and the existing children use 79, so ~13 px was being absorbed by
`qStripRow_`'s trailing stretch. An 8 px meter + 4 px spacing fits into that
slack and squeezes nothing.

## Two things found along the way

**The Track Detail dock's volume slider was broken** (fixed in M4). It was wired
to *nothing* — dragging it silently did nothing — and it mapped dB to pixels as
`value = dB * 10`, ignoring the arranger's `VOLUME_CURVE_EXPONENT = 0.5`
power law, so the same dB sat at two different positions in two views. There is
now ONE curve, `app/timeline/sfadercurve.h`, used by both, and the dock's slider
commits through `SSetTrackVolumeAction` like the arranger's. A meter next to a
lying fader is worse than no meter.

**The legacy pull path does not observe a post-freeze track-gain change.**
`twStreamingLatch::copyData` gates its cached page on the **`twPluginChain`'s**
content epoch, and `STrack::invalidateRenderPath()` does not reach the chain —
the same "an `SPluginChain` is not an `SLink` child of its track" pitfall
`plugins/CONTRACT.md` records for slots. Verified: an offline render tracks every
gain change correctly (0.203 → 0.287 → 0.203 for −6/−3/−6 dB) because the
scheduler re-plans and re-binds, while a direct `requestPage` after a second gain
change serves the first gain's audio. **Not a product bug** — playback and render
both go through the scheduler — but it shapes the tests, so `meter_postfader.qxa`
uses two tracks at different gains rather than changing one track's gain twice,
and the caveat is recorded in `smetertestactions.cpp` and `testkit/CONTRACT.md`.
A candidate retirement for `plan/proposed/20_DATAFLOW_FOLLOWUPS.md`.

## Gates

- `ctest -R metering_test` — 48 assertions: the span scan, the ballistics
  (including the frame-rate-independence invariant), and the probe's window
  arithmetic, page-boundary clamp, miss paths and stale-page acceptance. Links
  `tw_metering` ONLY, so a layering regression makes the test itself stop linking.
- `qxa.meter_levels` — the ramped-sawtooth fixture's per-second RMS
  (.067/.176/.291/.405) read at four positions; the past-the-end silence case;
  the density rules via the REAL head built off screen
  (`headHeight` 140 → `orient=v`, 80 → `orient=h`, 20 → `vis=0`); and three PNG
  grabs, the only coverage of `SLevelMeter::paintEvent`.
- `qxa.meter_postfader` — two tracks, same sample, unity vs −6.02 dB: the reading
  halves. Isolates the fader as the only difference.
- Full suite 100/100 (97 before). `check_layering.py` + `check_logging.py` clean.
  Flake gate: both new cases 15/15, and `meter_levels` 5/5 under
  `SMARAGD_REVAL_WORKERS=0` (legacy pull).
- Render byte-exactness: no milestone touches rendering or the RT callback.

## Deliberately not done

Numeric dB readout; armed-track input metering while stopped; stereo meters
(gated on the mixer growing to two busses AND the mono-page rule); compensating
the *playhead* for latency too (a one-line follow-up if wanted); a dedicated
mixer window; K-weighted / LUFS loudness. Also out of scope and pre-existing: the
per-callback `std::vector` heap allocation in the `twSpeaker` RT lambda
(`twspeaker.cc:185`) — a real RT-allocation bug, but not this feature's.
