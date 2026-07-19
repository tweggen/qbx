# Proposal 21 (DRAFT v2): Real-time data flows in the demand-driven dataflow

> **Status: DRAFT v2 (2026-07-19).** Architectural outline only — no code yet.
> v2 supersedes v1's "minimal monitor strip summed at the speaker": the user
> requirement is that a live input is heard through **the same signal flow it
> would have in playback** — the track's inserts, its routing, the busses,
> the master chain, the same component instances. v1's strip-sum is demoted
> to an optional low-latency degraded mode (§8).
>
> Prerequisite reading: `19_ASYNC_FREEZE_MODEL.md` ("Phase 2 REVISED" + the
> execution-class taxonomy), `20_DATAFLOW_FOLLOWUPS.md`, and the existing
> recording machinery (CLAUDE.md § Recording Audio; `AudioInput`,
> `RecordingSession`, `place-recording`).

---

## 1. The problem: two time domains, one signal path

The dataflow is **pull-from-the-past**: pages are deterministic functions of
known content, demanded and rendered ahead. Live input is
**push-from-the-present**: it exists once, arrives on the device clock, and
must be audible within milliseconds — through the SAME plugins and busses
that will process it on later playback. Pre-rendered master pages cannot
contain a signal that does not exist yet, so wherever a live signal JOINS
the shared graph, everything DOWNSTREAM of that join must be computed live.

This yields the central concept:

## 2. The live horizon (merge-point) principle

At any moment the component graph is split by a **live horizon**:

- **Upstream of the horizon — the frozen lane (unchanged):** deterministic
  content, rendered ahead as pages by the scheduler exactly as today.
- **Downstream of the horizon — the live lane:** computed at device-block
  granularity in real time, by the SAME component instances, via the
  existing block-pull contract (`calcOutputTo`).

The horizon's position follows from what is armed:

```
nothing armed:      [====================== frozen =====================]▶ speaker
                     (today's model — horizon at the speaker, live lane idle)

track T armed:       other tracks: [chains frozen as pages]──┐
                                                             ▼
                     input ▶ [T's inserts LIVE] ──▶ [rewire/busses LIVE] ▶ [master LIVE] ▶ speaker
                     (horizon sits at T's input; T's chain and every shared
                      component T feeds — busses, master — run block-wise)
```

Key consequences:

1. **The armed track's chain runs live on its normal instances.** No dual
   instances, no monitor-only FX subset: while armed, the frozen lane does
   not render this track (the v1 `liveExcluded_` exclusion), so its
   pluginchain/inserts are EXCLUSIVELY owned by the live lane — instance
   exclusivity falls out of the lane switch itself, the same way the
   scheduler's class-1 lanes own instances.
2. **Shared downstream (rewire, busses, master) runs live while anything is
   armed.** Its unarmed INPUTS are served from frozen pages: this is
   exactly what `twStreamingLatch::copyData` already does — position-
   aligned reads of ready pages. The expensive upstream work (every unarmed
   track's full chain) stays pre-rendered; only [armed chains] + [shared
   bus/master chains] execute per block.
3. **Monitoring is playback.** The audible path while armed is the identical
   component/plugin topology, instances, and parameter state that playback
   uses — the user requirement, by construction rather than by imitation.

## 3. Goals / non-goals

**Goals**
1. Live input audible at small-block latency THROUGH the full playback
   signal flow (inserts → routing → busses → master), same instances.
2. All dataflow invariants preserved: the RT device callback never freezes
   (`twRtThreadGuard`); no demand ever waits on the future; goldens stay
   byte-identical whenever nothing is armed.
3. The live signal continuously becomes ordinary frozen content (capture
   bridge → pages + WAV → `place-recording` on disarm).
4. Live plugin instruments enter as class-1 live mode; recorded events
   (prop 12) enable later deterministic re-render.

**Non-goals (this proposal)**
- Sub-block latency / hardware direct monitoring.
- Cross-device clock-drift correction (same-device duplex first).
- Live time-stretch of the input.

## 4. Execution model: the live graph thread

The device render callback must stay allocation-free and must never freeze.
Plugin chains and copyData (which may miss a page) are not that. Therefore
the live pull does NOT run in the device callback:

- **`LiveGraphPump`** — one high-priority thread (near-RT; above the
  readahead, below the device thread) that, while the live lane is active,
  pulls the graph OUTPUT block-by-block, a small fixed lookahead (1–2
  device blocks) ahead of the callback:
  `masterOut.calcOutputTo(block)` → pushes into an SPSC **liveMixRing_**.
- **Device callback:** when the live lane is active, it pops liveMixRing_
  instead of reading frozen pages; when inactive, today's frozen-page path.
  Pop-only, lock-free, RT-safe either way. Ring underrun ⇒ silence for that
  block + xrun counter (never blocks).
- **Inside the pump's pull:**
  - The armed track's input component serves device-input samples from the
    input ring (§5) — the block-pull face of `twLiveInputSource`.
  - Unarmed inputs of shared components serve from frozen pages via
    `copyData`. A missing page (readahead behind) must NOT synchronously
    freeze on the pump either — the pump thread gets a "no-render" guard
    like the RT guard (serve silence + poke the readahead), keeping the
    scheduler the only renderer. (Generalize `twRtThreadGuard` into a
    per-thread render-policy flag: RT = never; pump = never + poke.)
  - Position: the pump advances the playhead-aligned block cursor; seeking/
    loop-wrap resets it with the same discontinuity semantics as pages.
- **Latency budget:** input block + ring hop + output block (typ. 3 blocks;
  at 128 frames/48 kHz ≈ 8 ms) — standard software-monitoring territory.
  The pump lookahead adds robustness, not latency (it runs ahead of the
  consumer, bounded by the ring size).

**Arming transitions (the lane switch, now graph-wide):**
- **Arm:** mark track `liveExcluded_` (leaves the frozen mix; range
  invalidation as an ordinary edit); quiesce scheduler nodes touching the
  now-live-owned chain components (the `retireObject`-style drain applied
  to their in-flight nodes); reset+seek the downstream shared chain at the
  playhead; start the pump; flip the callback's source to liveMixRing_.
- **Disarm:** stop the pump; flip the callback back to frozen pages;
  clear `liveExcluded_`; invalidate the downstream range from the arm
  point (bus/master state diverged while live — their pages re-render);
  `place-recording` places the captured clip (existing endpoint).
- Both transitions are edit-path operations on the UI thread with a small
  audible discontinuity budget (like seeking today).

## 5. Components

### 5.1 `twLiveInputSource` — class-0, two faces

- **Block face (live lane):** `calcOutputTo` serves the device-input SPSC
  ring (written by the input callback). Underrun ⇒ silence.
- **Page face (frozen lane):** pages published by the capture bridge behind
  `captureFrontier_` (atomic). `planPage` = no deps; scheduler contract:
  live-bound nodes complete IN-LINE (published page, or silence-valid
  beyond the frontier) — no demand ever parks on the future (assert).
- `usesSerialCursor() = false` for the page face (immutable history).

### 5.2 The capture bridge (non-RT) — unchanged from v1

One input pump per active input, three sinks:
- the live-lane input ring (§5.1 block face),
- page assembly → timeline stamping (§6) → publication into the page face
  → `captureFrontier_` advance (waveform preview grows while recording;
  post-stop playback serves from cache),
- the WAV writer (durable artifact; `place-recording` consumes it as
  today). `RecordingSession` refactors into a consumer of this bridge.

### 5.3 `twLiveInstrument` — class-1 in live mode

A real-time plugin instrument is the class-1 exclusive instance entered
through LIVE mode: driven inside the live graph pull (its position in the
chain is wherever the user inserted it), consuming live MIDI from an event
ring. Captured like an audio input (audio pages + optionally the MIDI
events into a prop-12 event timeline). **Scheduled mode later:** the same
instance flips to scheduler-lane class-1 rendering (runs + reset/pre-roll)
from the recorded events — deterministic, editable re-render. Mode switch,
not architecture switch.

## 6. Time stamping & latency (unchanged from v1)

- Capture anchor: playhead at record start MINUS configured/reported input
  latency (per-device "recording offset" setting; measured round-trip
  calibration later). The input clock rules the captured material.
- Same-device duplex assumed first; separate-device drift affects only the
  live monitor alignment (capture stays internally consistent) —
  compensation is phase P5.

## 7. Scheduler semantics while the live lane is active

- The frozen lane KEEPS RUNNING for everything upstream of the horizon
  (unarmed tracks' chains) — the readahead demands pages as today; the
  pump's copyData consumes them.
- Components downstream of the horizon (busses/master) are NOT page-
  rendered while live (their nodes would fight the pump's instances):
  the demand planner treats them as live-owned — the readahead/preview
  demands stop at the horizon (plan-level exclusion mirroring
  `liveExcluded_`, but for the downstream shared set). Preview of the
  master waveform during arming is simply stale (acceptable; UI hint).
- Offline render while armed: renders the frozen world only (no live
  future); UI-level warning as in v1.

## 8. Degraded / optional modes

- **Low-latency direct monitor (v1's strip):** input ring → gain/pan →
  speaker sum, bypassing all chains. Optional per-track toggle for users
  who prefer minimal latency over through-chain sound, and the automatic
  fallback when a chain contains a plugin unfit for block-live execution.
- **RT-capability policy:** inserts run in the live pull only if flagged
  RT-capable (no allocation/locks in process; plugin ABI metadata,
  proposal 08). A chain with an unfit plugin monitors in degraded mode
  (plugin bypassed or strip-only) with a UI badge.

## 9. Threading contract (summary)

- Device input callback: writes input ring(s). Lock-free.
- Device output callback: pops liveMixRing_ (armed) or reads frozen pages
  (unarmed). Never renders (`twRtThreadGuard`).
- LiveGraphPump: block-pulls the live-owned subgraph; serves unarmed inputs
  from pages via copyData under a "never render, poke readahead" policy;
  never takes `queueLock_` while holding component locks (same ordering
  rule as everywhere).
- Capture thread(s): page assembly + publication + WAV; may take component
  locks for publication.
- Scheduler/workers: unchanged; horizon-excluded components get no nodes
  while live; live-bound page nodes complete in-line.
- UI thread: arm/disarm = ordinary edit-path operations (blocking reads per
  the `getDurationBlocking` rule) + pump start/stop + drains.

## 10. Reuse map

| Existing | Role here |
|---|---|
| `calcOutputTo` block-pull contract | the live lane's execution mechanism (why we never deleted it) |
| `twStreamingLatch::copyData` page reads | unarmed inputs of live-owned components |
| `AudioInput` backends | device seam; bridge pumps it |
| `RecordingSession` / `place-recording` / takes | capture sinks + disarm endpoint |
| `twRtThreadGuard` | generalized to per-thread render policy (RT: never; pump: never+poke) |
| prop 19 execution classes | class 0 input, class 1 live instruments, lane ownership |
| prop 16 fallback semantics | ring underrun / missing page ⇒ silence, never block |
| prop 12 event timelines | recorded MIDI for scheduled re-render |

## 11. Phasing (each independently shippable + gated)

1. **P1 — Live graph pump, audio input through the chain.** Arm → input
   heard through the track's inserts + busses + master, mixed with frozen
   playback of other tracks. Gates: goldens byte-identical when nothing is
   armed; RT guard + pump-policy guard clean; manual latency check; xrun
   counter ~0 on an idle machine.
2. **P2 — Capture bridge unification** (pages + WAV + preview-while-
   recording; `RecordingSession` refactor). Gate: recorded WAV
   byte-identical to the pre-refactor recorder on a deterministic test
   input; placement tests unchanged.
3. **P3 — Live plugin instruments** (MIDI event ring; depends on prop 08
   instrument ABI + RT-capability metadata).
4. **P4 — Event capture + scheduled re-render** (prop 12 integration; the
   live↔scheduled instrument mode switch).
5. **P5 — Multi-device & drift** (separate in/out devices, monitor-path
   drift compensation, measured latency calibration).
6. **P6 — Degraded modes polish** (direct-monitor toggle, unfit-plugin
   fallback + badges).

## 12. Open questions

1. Horizon computation for arbitrary routing: with sends/sub-busses, the
   live-owned set = all components reachable downstream from any armed
   source — needs a small graph walk on arm/disarm (and on rewiring while
   armed).
2. Pump block size vs device block size (fixed small block with internal
   re-blocking, or follow the device?). Plugins prefer stable block sizes.
3. Bus/master state at the arm boundary: reset (click-free ramp?) vs
   carrying state from the last frozen page (restoreInternalState from the
   page at the playhead — attractive: seamless arm during playback).
4. Two armed tracks on different busses: one pump pulling the whole live-
   owned set (single-threaded live graph) vs per-bus pumps (ordering).
   Start single-pump; the live-owned set is small.
5. Automation during live monitoring: parameter changes apply to the live
   instances immediately (they are the same instances — should just work;
   verify the edit-path locking budget per block).
6. Loop-recording/punch semantics at the frontier (prop 17 takes per pass).
7. Whether the pump should ALSO feed the capture bridge post-chain
   (recording the processed signal, "print FX") as an option vs always
   capturing the raw input (current design: raw input; print-FX later via
   freeze-in-place).
