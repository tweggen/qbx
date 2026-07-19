# Proposal 21 (DRAFT): Real-time data flows in the demand-driven dataflow

> **Status: DRAFT (2026-07-19).** Architectural outline only — no code yet.
> Prerequisite reading: `19_ASYNC_FREEZE_MODEL.md` ("Phase 2 REVISED" + the
> execution-class taxonomy), `20_DATAFLOW_FOLLOWUPS.md`, and the existing
> recording machinery (CLAUDE.md § Recording Audio; `AudioInput`,
> `RecordingSession`, `place-recording`).
>
> Goal: tracks whose content originates NOW — a live audio input (mic,
> line-in) or a real-time plugin instrument driven by live MIDI — playing
> alongside the page-frozen project, and becoming ordinary project content
> the moment they stop being live.

---

## 1. The problem: two time domains

The dataflow we just built is **pull-from-the-past**: every page is a
deterministic function of already-known content, so it can be demanded,
rendered ahead of the playhead, cached, re-rendered, and byte-compared.
Live input is **push-from-the-present**: samples exist only once, arrive on
the device's clock, cannot be demanded ahead of time, and must be audible
within milliseconds — while a page is ~1.37 s (65536 frames @ 48 kHz).

No scheduling cleverness reconciles these inside one lane: a demand for a
live page of the future is a request for information that does not exist.
The design therefore does not try. It runs **two lanes with a bridge**:

```
                     ┌────────────────────────────────────────────┐
 device input ──────▶│ LIVE LANE (RT): SPSC ring → monitor strip  │──┐
 (or live plugin     │ (gain/pan; RT-safe; NEVER freezes)         │  │  sum in the
  fed by MIDI)       └───────────────┬────────────────────────────┘  ├─ speaker
                                     │ same samples                  │  callback
                     ┌───────────────▼────────────────────────────┐  │
                     │ CAPTURE BRIDGE (non-RT thread):            │  │
                     │ assemble pages, stamp timeline positions,  │  │
                     │ publish to the live component's page cache,│  │
                     │ advance captureFrontier_, feed file writer │  │
                     └───────────────┬────────────────────────────┘  │
                                     │ frozen pages (the past)       │
                     ┌───────────────▼────────────────────────────┐  │
 rest of project ───▶│ FROZEN LANE (unchanged): scheduler demands,│──┘
                     │ page DAG, readahead, prop-16 fallback      │
                     └────────────────────────────────────────────┘
```

**The pivotal rule — arm/disarm is a LANE SWITCH, not a mode inside a lane:**

- **Armed / monitored:** the track's live source is audible ONLY through the
  live lane, and the track is EXCLUDED from the frozen mix (else
  double-audio). The frozen lane keeps rendering everything else.
- **Disarmed / stopped:** the captured region is ordinary page-frozen
  content (it already sits in the component's page cache and/or a placed
  clip via `place-recording`), and the track rejoins the frozen lane. The
  live lane is empty.

This mirrors how conventional DAWs treat input monitoring, and it keeps both
lanes internally pure: the frozen lane stays deterministic and demandable;
the live lane stays RT-safe and trivial (ring + strip + sum).

## 2. Goals / non-goals

**Goals**
1. Hear a live input (or live plugin instrument) mixed with frozen-page
   playback at block-size latency, without violating any dataflow invariant
   (RT never freezes; nothing in the graph waits on the future).
2. The live signal becomes ordinary frozen content continuously — waveform
   preview grows while recording, and playback-after-stop needs no import
   step beyond what `place-recording` already does.
3. Live plugin instruments are the class-1 story from proposal 19 §
   execution classes, entered through a LIVE mode: same instance, two modes
   (live lane now; scheduler lane later, re-rendering recorded events).
4. Reuse, don't duplicate: `AudioInput`, `RecordingSession`,
   `place-recording`/takes, `twCapturingSource`, the class-0/class-1 design.

**Non-goals (this proposal)**
- Latency below one device block (no ASIO-style direct monitoring).
- Cross-device clock-drift correction (same-device duplex first; drift is a
  listed follow-up).
- Non-audio live data other than MIDI-shaped events.
- Varispeed / live time-stretch of the input.

## 3. Components

### 3.1 `twLiveInputSource` — the class-0 component

The engine-side face of one live input channel(s). It is a `twComponent`
whose pages come ONLY from the capture bridge:

- `captureFrontier_` (atomic uint64): timeline position up to which pages
  are published. Grows monotonically while capturing; frozen thereafter.
- `planPage(pageStart)`: no deps. Scheduler semantics (see §6): a node at
  `pageStart + capacity <= captureFrontier_` completes with the published
  page; a node beyond the frontier completes IMMEDIATELY as silence-valid —
  it never parks a demand on the future. (While armed this is moot for
  audibility: the track is excluded from the frozen mix.)
- `usesSerialCursor() = false` — published pages are immutable history;
  reads are pure.
- Rendering methods (`renderFrames`) serve from published pages only; the
  component never touches the device.

### 3.2 The monitor strip + live mix stage (RT)

- Device input callback (or duplex render callback) writes into a lock-free
  SPSC ring (`monitorRing_`, small — 2..4 device blocks for latency).
- The speaker render callback, AFTER producing the frozen-page mix, pops the
  ring, applies the track's strip (gain/pan — RT-safe subset only; no
  allocation, no locks, no freezing; `twRtThreadGuard` remains satisfied
  because nothing here can reach `freezePage`) and sums into the output.
- One monitor slot per armed track; a fixed-size array of slots
  (`kMaxLiveLanes`, e.g. 8) so the callback iterates without allocation.
- Underrun policy: ring empty → the live contribution is silence for that
  block (never blocks, never stretches).

### 3.3 The capture bridge (non-RT)

A dedicated capture thread per active input (grown from today's
`RecordingSession` loop):

- Pulls from `AudioInput::read` (as today) into a LARGER capture ring
  (robustness; independent of the monitor ring so a UI stall can't cause
  monitor xruns and vice versa).
- Assembles page-aligned buffers, stamps them with TIMELINE positions
  (§5), publishes each completed page into the `twLiveInputSource` cache
  (`setPageAsFrozen`-style + `stalePredecessor` unused; epoch = live
  component epoch), then advances `captureFrontier_`.
- Simultaneously feeds the existing WAV writer (the recorded file remains
  the persistent artifact `place-recording` consumes — publication to pages
  is an ADDITIONAL sink, giving live waveform preview and instant post-stop
  playback from cache while the WAV is the durable copy).
- **Refactor:** `RecordingSession` becomes a consumer of this bridge
  (monitor ring + page publisher + file writer are three sinks of ONE input
  pump) instead of owning the input pull loop privately.

### 3.4 `twLiveInstrument` — class-1 in live mode

A real-time plugin instrument (live MIDI → audio) is the exclusive-instance
class-1 component of proposal 19, entered through a LIVE mode:

- **Live mode:** the instance renders in the live lane — driven by the
  device callback cadence (or a dedicated near-RT thread one ring ahead),
  consuming live MIDI events from an event ring. Its audio goes to the
  monitor strip AND the capture bridge exactly like an audio input. It is
  absent from the frozen mix while live.
- **Event capture:** in parallel, the raw MIDI events are recorded
  (timestamped in timeline positions) into an event timeline (proposal
  12's structure is the natural home).
- **Scheduled mode (later):** after recording, the same instance flips to
  the scheduler lane: a class-1 lane renders pages from the RECORDED event
  timeline with runs + reset/pre-roll repositioning (deterministic
  re-render, editable MIDI). Audio-capture users can instead keep the
  bounced pages (freeze-in-place semantics, proposal 20 §4).
- The mode switch is a per-component state, not an architecture switch —
  the point of the execution-class design.

## 4. The armed-track exclusion

While a track is armed/monitored, its contribution must come from the live
lane only:

- `twTrackMix` gains a `liveExcluded_` flag (set on arm, cleared on
  disarm), honored by `freezePage_nolock`/`calcOutputTo` and by
  `planPage` (plans no deps for excluded clips). Distinct from user mute so
  UI state is not clobbered.
- Pages of the track/downstream chain rendered while excluded are correct
  (they simply lack the live contribution); on disarm, the arm/disarm edit
  bumps the affected range (ordinary `twEditRange` invalidation) so the
  chain re-renders WITH the new clip placed by `place-recording`.
- Renders/exports during arming: the frozen lane renders what it knows —
  live-ahead-of-now is silence. Exporting an armed track is a UI-level
  warning, not an engine error.

## 5. Time stamping & latency

- **Timeline anchor:** capture start maps device-callback time to the
  playhead position at record start (as `RecordingSession` does today),
  MINUS the reported/configured input latency (new per-device setting,
  "recording offset"; measured round-trip calibration is a follow-up).
  Subsequent samples advance by count — the input device clock IS the
  ruler for the captured material.
- **Monitor latency:** input block + output block (duplex backends can
  do same-callback pass-through where available — WASAPI/CoreAudio duplex
  is a per-backend capability flag; otherwise one ring hop).
- **Clock domains:** same-device duplex assumed in phase 1 (one clock).
  Separate input/output devices drift; the capture side is still internally
  consistent (its own clock), so recorded material is correct — only the
  monitor mix and long-session frontier alignment drift. Drift compensation
  (micro-resampling the monitor path; periodic re-anchoring of the
  frontier) is phase 5.

## 6. Scheduler semantics for class-0 (the frontier contract)

The one dataflow-core change: the scheduler must never wait on the future.

- `twPagePlan` gains `liveBound` (or the component exposes
  `isRealTimeBound()`): expansion marks such nodes.
- A live-bound node executes immediately: serve the published page if
  `pageStart + capacity <= captureFrontier_`, else complete with a
  silence-valid page WITHOUT parking (dependents proceed; prop-16-style
  absence semantics, but explicit). NO demand ever blocks on a live
  frontier.
- Frontier advance publishes pages into the cache; consumers that care
  (preview reval, post-stop playback) find them by the ordinary demand
  path. An OPTIONAL frontier notification (bump a content-epoch-range on
  the live component as pages land) lets previews grow live without
  polling.
- Invariant to add to the stage-6 assert family: a `GraphDemand` must never
  have `outstanding_ > 0` solely due to a live-bound node (assert in
  expansion: live-bound ⇒ node completes in-line).

## 7. Reuse map

| Existing | Role here |
|---|---|
| `AudioInput` (WASAPI/ALSA/CoreAudio) | unchanged device seam; bridge pumps it |
| `RecordingSession` | refactored into the capture bridge (one pump, three sinks) |
| `place-recording` + takes (prop 17) | unchanged endpoint at disarm/stop |
| `twCapturingSource` / capture pages | the publication template for §3.3 |
| prop 16 stale/absent fallback | the "beyond frontier = silence" semantics |
| prop 19 execution classes | class 0 = `twLiveInputSource`, class 1 live mode = `twLiveInstrument` |
| prop 12 event timelines | recorded MIDI for scheduled re-render |
| `twRtThreadGuard` | already enforces "monitor path never freezes" |

## 8. Phasing (each independently shippable + gated)

1. **P1 — Monitor lane for audio input.** Arm → hear the input summed with
   frozen playback (monitor ring + strip + speaker-callback sum +
   `liveExcluded_`). Gate: standing proposal-20 gates untouched (goldens
   bit-identical — the frozen lane must be byte-unaffected when nothing is
   armed); manual latency check; RT guard clean.
2. **P2 — Capture bridge unification.** One input pump feeding monitor ring
   + page publication + WAV; live waveform preview grows while recording;
   post-stop playback serves from cache instantly. `RecordingSession`
   becomes a bridge consumer. Gate: recorded WAV byte-identical to the
   pre-refactor recorder for the same input fixture (loopback test input —
   add a `NullInput`-style deterministic test source); placement tests
   unchanged.
3. **P3 — Live plugin instruments (audio path).** `twLiveInstrument` in
   live mode fed by a MIDI event ring; monitored + captured like P2.
   Depends on plugin hosting (proposal 08) providing an instrument ABI.
4. **P4 — Event capture + scheduled re-render.** Record MIDI to an event
   timeline (prop 12); flip the instrument to scheduler-lane class-1
   rendering (runs + pre-roll) from recorded events; captured-audio bounce
   remains the alternative (freeze-in-place).
5. **P5 — Multi-device & drift.** Separate input/output devices; drift
   compensation for the monitor path; measured round-trip latency
   calibration.

## 9. Threading contract (summary)

- Device input thread: writes monitor ring + capture ring. No locks, no
  allocation.
- RT output callback: frozen-page pull (unchanged) + monitor-ring pops +
  strip + sum. Never freezes (`twRtThreadGuard` enforced), never touches
  the capture bridge.
- Capture thread (per input): page assembly, timeline stamping, cache
  publication, frontier advance, file writes. May take component locks
  (publication) but NEVER the scheduler's `queueLock_` while holding a
  component lock (same ordering rule as expansion).
- Scheduler/workers: unchanged; live-bound nodes complete in-line, no new
  blocking.
- UI thread: arm/disarm edits (lane switch + `liveExcluded_` + range
  invalidation) are ordinary edit-path operations (blocking reads per the
  `getDurationBlocking` rule).

## 10. Open questions

1. Duplex capability per backend (same-callback pass-through vs ring hop):
   WASAPI shared-mode duplex, CoreAudio AggregateDevice behavior, ALSA
   full-duplex — needs a per-backend capability probe.
2. Monitoring THROUGH plugin inserts (input → insert chain → monitor): the
   inserts would run in the live lane and must be RT-safe — intersects the
   class-1 lane design; likely restricted to a "monitor FX" subset first.
3. Multiple simultaneous inputs: `kMaxLiveLanes` sizing; per-input capture
   threads vs one multiplexed pump.
4. Punch-in/out and loop-recording semantics on the frontier (takes per
   loop pass — prop 17 interplay).
5. Should the frontier's silence-beyond semantics be surfaced in the UI
   during arming (e.g. hatched region ahead of the playhead)?
6. Whether monitor-lane strips share the twTrackMix gain/pan state or a
   dedicated RT-safe mirror (avoid taking the trackmix mutex in the
   callback — likely an atomic snapshot like SCut's).
