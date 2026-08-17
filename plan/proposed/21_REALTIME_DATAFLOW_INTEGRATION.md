# Proposal 21 (DRAFT v3.1): Real-time data flows in the demand-driven dataflow

> **Status: DRAFT v3.1 (2026-08-17).** v3.1 applies an adversarial review of v3
> (§13: 3 blockers, 13 majors, ~20 minors — each answered inline). The blockers
> were real: v3 had no transport model for the pump (what position a block is
> rendered at while stopped vs playing, and how the sequenced feed is masked
> while stopped); MIDI-thru would have made the pump a second producer on the
> single-producer scheduler ring; and "live-only output state" was a rewrite of
> `twSpeaker`'s lifecycle disguised as a flag. v3 (2026-08-17) superseded v2
> (2026-07-19), which predates proposals 36 and 37 — v2's central mechanism
> ("run the same instances block-wise via `calcOutputTo`") is dead: mono,
> deprecated, and holding component mutexes across recursion. What survives, one
> level lower, is the plugin *processor's* block renderer plus two pure functions
> (gain, channel map), and that is enough. Written from three concept studies
> (engine, app, industry) and the review. Execution companion:
> `21_ORCHESTRATION.md`.
>
> Prerequisite reading: `19_ASYNC_FREEZE_MODEL.md` ("Phase 2 REVISED", execution
> classes), `20_DATAFLOW_FOLLOWUPS.md`, `36_MULTICHANNEL_SIGNAL_FLOW.md` §4,
> `37_MIDI_INSTRUMENTS_AUTOMATION.md` D4/D6/§3.2.1/§4.3/§4.4, `docs/contracts/
> {THREADING,FREEZE_PROTOCOL}.md`, `tw303a/{playback,schedule,plugins,devices,
> record}/CONTRACT.md`, `35_ASIO_BACKEND.md` (the hardware prerequisite for
> low-latency numbers — §4).

---

## 0. Scope

| # | Capability | Industry anchor |
|---|---|---|
| A | **Live audio input** heard through the armed track's inserts → its parent folders → the master, on the same plugin instances playback uses, while the rest of the arrangement keeps playing from frozen pages | Cubase ASIO-Guard: "record-enabled/monitored channels and all dependent channels switch to real time" with a crossfade; REAPER anticipative FX disabled per armed track |
| B | **Live MIDI input** (hardware port or the computer keyboard) into the armed track's instrument, merged with its sequenced notes when playing, MIDI-thru to the track's port | every reference DAW; Cubase VSTi record-enable → real-time path |
| C | **Audio recording** through one input pump: monitor ring + wide capture pages (waveform grows while recording) + WAV, placed at stop with proposal 17's takes; per-device recording offset; punch/loop | REAPER/Cubase/Logic/S1 |
| D | **MIDI recording** to a new/overdub/replace `SMidiCut` with takes per loop pass, timestamps → ticks, input quantise at commit; retrospective capture later | Cubase Retrospective Record, REAPER retroactive record |
| E | Transport support: monitor modes Off/On/Auto, count-in, metronome click on the live path, latency readout | universal |

Not in scope: sub-block / hardware direct monitoring; cross-device clock drift
(L6 outline); live time-stretch of input; input FX chains ("record wet");
sends/sub-busses (do not exist yet — when they do, they join the closure by the
same rule); a master insert chain (would move D2 to its option (b) — named, not
built); the ASIO backend itself (proposal 35); hearing an armed audio track's
OWN clips live (needs proposal 20 §2; §10.1).

---

## 1. What is true today (facts that shape the design)

**Engine**
- F1. **The block pull is mono, deprecated, and locks across recursion.**
  `twStreamingLatch::copyData` clamps to the latch index (graph inv. 12);
  `calcOutputTo` is on proposal 20 §3's deletion list; a plugin's legacy pull
  feeds channel 0 only and holds `pluginsMutex_`/`mutex()` across whole pulls;
  an instrument's legacy pull renders silence (plugins inv. 42).
- F2. **What survives block-wise, wide:** `twPluginSlotProcessor::render(in, out,
  len, startPos, positional=true, sr)` — any block length (chunked to 4096, short
  last chunk legal), state-continuous when `startPos == lastEnd_`, otherwise a
  REPOSITION (reset + chase + pre-roll ≥ 4096 frames of discarded DSP,
  `twpluginslotproc.cc:955-964`), the sequenced FEED collected at `[startPos,
  +len)` and automation applied per chunk; `twProcessContext.playing` is
  hard-wired `true`. `twGainStage::applyGain(src, dst, n, pos, envelope)` and
  `twRewire::channelMap()` are pure in position. `Transparent`/`Missing` slots
  already pass through inside `runChunked_nolock`.
- F3. **`renderPageWide(page, frames < FRAME_CAPACITY)` is accepted, but its input
  side is page-shaped** (`fetchInputPage(plug, page.startPosition)` freezes a
  FULL page at an unaligned start on a miss); small pages per lane are not
  viable (`FRAME_CAPACITY` in 99 places / 28 files; alignment baked into
  `copyData`, the RT cursor, the readahead, the scheduler, `planPage`, cache
  keys, `previousPage`, meter probes).
- F4. **The master is a unity sum plus an identity map**: `SStdMixer` builds one
  `twMixer` and an identity `twRewire` root, no master chain, no sends. A track
  is `twTrackMix → twPluginChain → twGainStage → twRewire`, all wide.
  `SStdMixer::reconnectTracksToMixer` nulls the input plug of an inaudible
  top-level track; a nested lane's audibility is `twTrackMix::setClipMuted`
  (`applyChildTrackAudibility`); `planPage` skips a nulled plug. Stale root
  pages are REPLACED, not re-rendered in place, and the RT adopts a fresh page
  by epoch (`updateFrozenPage`).
- F5. **The RT callback** reads root pages by position with the stale fallback
  (proposal 16), fills N planar buffers (`pullBlock`), `twSpeaker` maps them
  stereo onto the device (playback inv. 9). `twSpeaker::startOutput()` returns
  if already playing, mints a NEW `AudioEngine` per start, registers the
  callback, defers the device start until the readahead has primed ~3 s
  (`minBufferFrames_` = 144000), and the callback zero-fills unless the engine
  is PLAYING; `stopOutput()` closes the device. `CaptureBackend::startOutput()`
  clears its recording + block log ("frame 0 = the current playback session",
  testkit rule 1). The RT publishes the position only while PLAYING; the MIDI-out
  pump's host-time anchor is taken on that publication.
- F6. **Latency numbers.** WASAPI shared: `kRequestedDurationHns` = 100 ms,
  variable callback block, `outputLatencyFrames` from `GetStreamLatency`; the
  capture backend is a fixed 1024-frame block. v2 §4's "8 ms" is not reachable
  here; the honest budget is ~100 ms out + input until proposal 35 (ASIO).
- F7. **Input side.** `AudioInput::read` is a non-blocking poll (`RecordingSession`
  spins with a 1 ms sleep); `WASAPIInput` is NOT event-driven and its `read`
  releases a whole packet after copying only `framesToCopy` (drops the tail of
  a packet larger than the caller's buffer). `createAudioInput()` is a compile-
  time platform pick — no env selection, no file input. `MidiInput` (37 P7a)
  delivers on a device thread with host-time stamps; `CaptureMidiInput` exists,
  has `inject()`, and `SMARAGD_MIDI_BACKEND=capture` already selects it (and is
  defaulted under `--test-case`). `MidiOutScheduler::enqueue()` is SINGLE-
  producer, main thread only (devices inv. 11).
- F8. **Instruments (37)**: `forgetContinuity()`; the render barrier `beginRun`
  walks instrument tracks at render/play start; every generator page is a pure
  function of position + feed (plugins inv. 40); `STrack::syncInstrumentSlot()`
  calls `setEventSource(eventFeed())` from adopt/insert/remove, and
  `setEventSource` clears continuity AND bumps the param epoch; `twEventMerge`
  namespaces note ids per source (16 sources).
- F9. **Meters** read pages by position (34); `pumpMeters` ticks only while
  `isPlaying_ || isRecordingActive()`. `startRecording` sets `isPlaying_`
  directly (bypassing `setPlaying()`).
- F10. **Threading**: `twRtThreadGuard` is a per-thread "never render" marker
  (an `assert()` in `freezePage`); `CaptureRevalidator` has `pause()/resume()`
  (drains ALL in-flight jobs incl. import-time analysis) and `retireObject`
  (IRevalidatable only); nodes are dependency-counted and deduped in
  `graphNodes_`; asset captures / previews on the reval lane are NOT graph
  nodes; the readahead holds a single demand handle. Teardown:
  `smaragdOrderlyShutdown` (immortal `TwLog`, `stopScan()`), `MidiOutScheduler`
  joins Qt-free.
- F11. **The position-code decoder resolves 4096-frame blocks**
  (`decodePositionAt`, `position_code.h`); latency below that is invisible to
  `assert-source-position`.

**App**
- A1. Recording today: `startRecording` starts the record worker FIRST so
  `locatorHeldElsewhere()` makes it the playhead authority; "monitoring" is
  frozen playback; a modal dialog; `onRecordingCompleted` shifts the clip by
  `outputLatency − inputLatency` in DEVICE frames (no rate conversion);
  `place-recording` (takes for covered columns, one composite). No count-in
  (metronome is a stub), no punch, no loop record.
- A2. `SAutomationRecorder` (37 P6) = transport-bounded recorder, ONE action at
  stop; `SVirtualKeyboardDock` submits `add-note` at the locator (no release;
  `virtual-key` has only `durationTicks`); `SAddTakeAction` is audio-only.
- A3. Per-track attributes so far: `midiOutPort/Channel/OffsetMs`,
  `midiRouting`, `ArmedForRecording` (serialized). Options: audio input combo is
  a "System default" TODO; the MIDI page lists inputs but they are inactive.

---

## 2. The nine decisions

### D1 — The live lane is a *live executor* over processors, not a second component graph
A `LiveGraphPump` (one std::thread per active live lane) executes a **live plan**
— an immutable, main-thread-built snapshot of the live-owned subgraph — once
per device block: for each armed track, `input ring (or instrument events)` →
`processor->render(…, positional=true)` for every slot in slot order → `applyGain`
→ `channelMap`; for a folder parent in the closure, sum its live children with
its unarmed children's frozen root pages read **by position** (`getPageIfExists`
try-lock; miss = silence for that input this block, keep the previous page like
`twLevelProbe`) → the parent's own processors/gain/map; the top of the closure is
pushed into the SPSC `liveMixRing_` — **position-stamped** entries `{startPos,
flipEpoch, planar N × block}`, 3–4 deep. Steady state: no allocation after the
first blocks (the processor's event vectors are pre-sized; the plan carries
scratch for the widest track × device block). Rejected: (a) a wide block-pull
virtual on every component — the B1–B4 sweep again plus the lock recursion that
made the pull unsuitable for a near-RT thread; (b) small pages per lane (F3);
(d) v2's degraded strip — unnecessary, `runChunked_nolock` already passes
`Transparent`/`Missing` slots through (F2); RT-fitness metadata is future ABI
work.

### D2 — The live clock and transport model *(new in v3.1 — the blocker)*
The pump renders every block at a **position** and with a **feed policy** that
depend on the transport:

| Transport | Block position | Sequenced feed | Automation | Continuity |
|---|---|---|---|---|
| PLAYING | `livePos = enginePublished(seq, pos, hostNs) + leadFrames` (lead = ring depth); source: a new engine atomic read through `PlaybackContext`, never `SApplication` | ON (merge{feed, live}) | evaluated at `livePos` | contiguous by construction; a seek / loop wrap / play start is ONE explicit reposition (the plan tells the processor: `resetContinuity` + chase from the feed at the new position) |
| STOPPED (armed && monitoring) | a **virtual counter** `vpos = locator + blocks·n` (monotone, so `render` stays contiguous) | **MASKED** — the merge's sequenced sources are gated OFF (no DAW plays sequenced notes while stopped; chase happens on PLAY) | **held** at the locator's value (`holdAt(locator)` in the plan) | contiguous; the STOP↔PLAY transition is one reposition |

Both need one small, flag-gated addition to the processor: a **`twLiveTransport
{playing, feedEnabled, holdAutomationAt}`** consulted per chunk while
`liveOwned` (feed collection skipped when disabled; automation chase taken at
`holdAutomationAt`; `ctx.playing` reports the truth). This is the ONE change to
`render()` semantics, in L1a, gated by `liveOwned` so the frozen path is
byte-identical. The RT sums a ring entry only when (a) its `startPos` matches
the frame the RT is delivering (mismatch = drop-old / silence-on-miss +
counter) and (b) the served root page's `contentEpoch >= flipEpoch` — the
**epoch-gated flip**, because a stale root page still CONTAINS the newly armed
track's audio until the re-summed page lands (F4); a 2–3 ms crossfade smooths
the flip once the gate opens. While STOPPED there is no root page: `out = ring`.

### D3 — Exclusion by the existing wiring rule; RT = frozen root page + live ring; exact by linearity
The **topmost closure member** (the armed track itself, or the highest folder in
its closure) is made inaudible to the frozen sum by exactly the rule solo uses at
the mixer (`reconnectTracksToMixer` nulls its plug); **nested members** of the
closure are `setClipMuted` in their parent's `twTrackMix`; the mixer/root epoch
bumps; the master re-sums the remaining roots (milliseconds per page); `planPage`
skips the nulled plug — **no node is planned for the live-owned chain**, so
exclusive ownership of its processors is by construction for new demands. Every
UNARMED sibling under the excluded folder is then rendered by the pump from its
frozen root pages by position — which is why the readahead **re-roots demands**
at every "frozen input of the live plan" (`requestGraphPages(siblingRoot, pos, n,
prio 9)` — one demand handle per root; supersession per handle); the pump never
demands. The RT keeps reading root pages exactly as today and adds the ring:
`root(unarmed) + live(closure) == root(all)` sample-for-sample because the
master is a unity sum with an identity map (F4). The excluded folder's OWN clips
are silent while a child is live (stated; §10). `isLiveOwned` is a separate
predicate applied at the mixer/track-mix wiring only — never folded into
`ssolo::isLaneAudible` (that would drop a live child's EVENTS from a folder
instrument's feed and darken meters). Rejected for now: the pump rendering the
whole master from N frozen roots (v2's shape) — zero invalidation on arm but it
duplicates the RT's stale-fallback logic and darkens the master meter; it
becomes the design the day a master insert chain exists.

### D4 — Instrument live mode is P3b's processor driven contiguously; ownership is a protocol
No `twLiveInstrument`. Arm (main thread): build the plan → apply the exclusion
wiring → **drain** in-flight nodes holding the chain's processors
(`CaptureRevalidator::retireComponentNodes`, §5) → `slot->forgetContinuity()` →
`proc->setLiveOwned(true)` (assert-first: a freeze-path `render` arriving while
live-owned answers silence and counts, so a missed drain — or a preview/asset
demand on the reval lane, which is not a graph node — is measurable, never a
corrupted voice) → the **live event source becomes a MEMBER of the track's
`eventFeed()` merge** while live-owned (never a processor-level
`setEventSource` swap: `STrack::syncInstrumentSlot()` re-applies the feed from
adopt/insert/remove and `setEventSource` clears continuity and bumps the epoch —
F8). The first live block is one reposition; every later block is contiguous.
`twLiveEventSource::collect` drains the MIDI ring, maps host time → project
frame with the live clock (D2; minus input latency), rebases, **clamps late
events to offset 0 (never drops)**, and keeps its own held-note table for the one
chase at live start. Disarm: last block flushed with all-notes-off → the live
member leaves the merge → `forgetContinuity()` → re-wire into the frozen sum →
`invalidateRenderPathRange(armPos, ∞)`. **Offline render while armed**: the
render suspends every live lane for its duration (`startRender` disarms
monitoring, instruments return to the frozen lane with the barrier, the lanes
resume after) — export ignores the split, as in Cubase; `beginRun` therefore
never meets a live-owned track.

### D5 — Latency honesty; the speaker gains a two-lane lifecycle; the input gets a real capture thread
WASAPI shared gives ~100 ms out; **proposal 35 (ASIO) is the hardware
prerequisite for software-monitoring quality** — 21 is correct at any block size,
only slow. `twSpeaker` becomes an explicit machine **device {CLOSED, OPEN} ×
frozen lane {IDLE, BUFFERING, PLAYING} × live lane {OFF, ON}**: `out = (frozen ==
PLAYING ? root : 0) + (live == ON ? ring : 0)`; arming while stopped OPENS the
device (live ON, frozen IDLE, callback running); Play attaches the frozen lane
without re-opening (a new engine may be minted under the running callback —
handle swap under the leaf `engineMutex_`, snapshot per callback as today; the
readahead priming gates the frozen lane's PLAYING, never the ring); Stop
returns the frozen lane to IDLE and leaves the device open while live is ON;
disarm-all closes it. `CaptureBackend` clears its recording at DEVICE start (not
play start); testkit rule 1 becomes "frame 0 = the current DEVICE session; cases
map through `frameAtHostTime`". `playback_test` gains a block per transition.
The input side gets an **event-driven capture thread per device** writing an
SPSC ring (WASAPI re-initialised with its own capture event; fixes F7's packet
drop); the pump pops. Round trip = input period + ring hop + pump lookahead
(1–2 blocks) + output buffer + reported latencies.

### D6 — One named placement conversion, anchored on the published position
`recordStart` is NOT "the locator at press": capture frame 0's host time is
mapped through the live clock's anchor (published position + publish-lag
correction, `SMidiOutPump`'s discipline) to a project frame `P0`, and capture
frame k lands at `P0 + k − inputLatencyProj − outputLatencyProj + userOffsetProj`
(all in PROJECT frames through the same scaling as `meterLatencyFrames()`).
Derivation, so the sign is not re-argued: the performer plays to what they HEAR,
which is the engine position emitted `outputLatency` earlier; the microphone's
sample reaches the ADC `inputLatency` before it is delivered; so the musical
moment of capture frame k is `positionDelivered(t_arrival(k)) − outLat − inLat`
— what REAPER's "use audio driver reported latency" (round trip) does; the
manual `recordingOffsetMs` per input device (`SSettings`) absorbs what the
driver misreports. Shared ANCHOR with 37 P7's MIDI-out, independent knobs.

### D7 — One input pump, three sinks; recording is a consumer of the bridge
Per active input: (1) the live-lane ring, (2) **wide** capture pages published
into a **`twGrowingCaptureSource`** (chunked planar storage, atomic frontier —
`twCapturingSource` is fixed-size and stays for containers) behind an
**`SRecordingContent`** (an SObject content whose duration grows; the recording
cut is an ordinary `SCut` over it; the arranger draws the frontier; preview
peaks are extended incrementally from the frontier, never recomputed; growth
emits `durationChanged` at ~10 Hz — cheap because the armed track is excluded
from the frozen lane), (3) the WAV writer, with backpressure that never stalls
the ring (a WAV falling behind drops to a "late" counter and the file is
finalised from the pages). `RecordingSession` becomes a ring consumer (no 1 ms
poll; non-modal — the dialog polls). At stop `place-recording` replaces the
temporary object with the final WAV-backed cut INSIDE the one macro (takes for
covered columns; loop record = one take per pass via the pump's wrap detection +
`add-take startOffset=` — proposal 17's "phase 5" falls out).
`locatorHeldElsewhere()` retires: the OUTPUT publication is the playhead
authority in every mode; `startRecording` goes through `setPlaying()`.

### D8 — MIDI: the recorder is a main-thread consumer of a tee; thru has its own immediate path
The MIDI input device thread writes ONE ring per consumer (a fan-out at the
callback: live-lane ring, recorder ring, thru) — SPSC stays SPSC. `SMidiRecorder`
mirrors `SAutomationRecorder`: bounded by the transport (`transportStarted/
Stopped`, which now includes record start), stamps host time → project frame
(shared `SPlayheadClock` extracted from `SMidiOutPump`) → ticks (tempo map,
once), and at stop commits **`place-midi-recording`** (planner shape of
`place-recording`: `add-midi-take` where a MIDI clip covers the column — new
verb, `add-take` is audio-only — else `insert-midi-clip` with events); modes
**new take / overdub / replace** (global setting); loop = one take per pass in
one macro; input quantise = a `quantize-notes` inside the macro; retrospective
capture later (`place-retro-midi`). All-notes-off + CC/sustain chase on stop/
locate. **MIDI-thru**: the device thread hands bytes to `MidiOutput` through a
second, dedicated **immediate SPSC ring on `MidiOutScheduler`** (device-thread
producer, scheduler-thread consumer, wakes the sender immediately) — never
`enqueue()` (main-thread single producer, devices inv. 11); budget = one sender
wake ≈ ≤ 2 ms measured, stated honestly.

### D9 — One per-track input selector; monitor Off/On/Auto; the computer keyboard is a real port; testable headlessly
`STrack::trackInput = none | audio:<device>:<channelMask> | midi:<port>:<channel|any>
| keyboard` (portable NAMES via `SSettings`, like `midiOutPort`; the head's arm
right-click menu; default derived from `instrumentSlot()`), `monitorMode = auto |
on | off` — **Auto = tape-machine style** (Cubase "Tapemachine", REAPER "auto"):
input while stopped or recording, playback while playing; **On** replaces the
track's playback with the input (Cubase semantics, not REAPER's sum) — serialized
when non-default; arm reuses `ArmedForRecording` (never starts monitoring on
load). Live set = closure of `{armed && monitorEffective} ∪ {monitor == on}` up to
the master. `SVirtualKeyboardDock` becomes an in-process `MidiInput` port
("Computer keyboard") so live play and recording treat it like hardware; step-
input at the locator stays the stopped-transport behaviour. Meters: the pre-FX
input meter is a lock-free peak pair from the bridge; a live track's post-FX
meter is published position-keyed by the pump; `pumpMeters` ticks while a live
lane is ON. Solo/mute of a live-owned track: mute silences its ring
contribution (structural mute stays), solo behaves as for any track. Testkit:
`SMARAGD_AUDIO_INPUT_BACKEND = file:<wav> | null | default` (`null` is the
`--test-case` default; `FileAudioInput` replays a WAV in 1024-frame blocks
**paced on the shared steady clock** `MidiOutScheduler::hostNowNs()`, with a
configurable reported input latency), the existing capture MIDI input fed by
`midi-in-event` / `midi-in-replay`; latency measured by **cross-correlation of
the known input file against the capture** (`assert-monitor-latency`, sub-block,
independent of the position decoder — F11); `assert-audio-continuity`,
`assert-render-policy`, `assert-recorded-clip`, `virtual-key hold/release`.
**Goldens byte-identical whenever no live lane is active** holds by construction
(the pump exists iff a live lane is ON — armed∪monitor∪metronome — and renders
suspend it).

---

## 3. The live horizon, precisely (post-36 graph)

| Case | Live-owned (rendered by the pump) | Frozen inputs read by position | Exclusion wiring | Notes |
|---|---|---|---|---|
| (i) armed AUDIO track, top level | its slot processors, gain, map; source = input ring | none | plug nulled at the mixer | its own clips silent while armed (§10.1) |
| (ii) armed INSTRUMENT track | slot-0 processor with `events = merge{feed (gated by transport), live}`, later slots, gain, map | none | plug nulled | sequenced notes sound only while PLAYING (D2) |
| (iii) folder with a live child | the folder's SUM (live children + unarmed children's frozen roots) + its processors + gain + map; the live child as (i)/(ii) | every unarmed child ROOT (re-rooted demands) | the FOLDER's plug nulled at the mixer; the live child `setClipMuted` in the folder's trackmix | the folder's own clips silent; child MIDI bubbling to a folder instrument makes the folder the live instrument and the child a MIDI source |
| (iv) sends / sub-busses | — | — | — | do not exist; closure by the same rule when they do |
| root | not live: `root(unarmed) + ring` in the RT, epoch-gated | the root page | — | exact by linearity; a master chain moves this to D3 option (b) |

Plan rebuild triggers: arm/disarm, `trackInput`/`monitorMode` change, transport
state change (feed policy), reparent/insert/remove of a track in the closure,
device change; the plan is swapped atomically, the old one released after the
pump's next block.

---

## 4. Threading contract

| Thread | New/changed | May | Must not |
|---|---|---|---|
| Input capture (per device) | NEW, event-driven | write its SPSC ring | touch Qt, block |
| MIDI input (device) | existing (P7a) | write its per-consumer rings; push the thru ring | touch Qt |
| **`LiveGraphPump`** | NEW std::thread, MMCSS "Pro Audio", `markLiveThread` | take a live-owned processor's `mutex_` (bounded), `getPageIfExists` try-lock, push the live ring, publish levels, read the engine position atomic | `freezePage`/`requestPage`/`fetchInputPage`/`copyData`, `requestGraphPages`, any blocking component `mutex()`, `queueLock_`, steady-state allocation, Qt |
| Readahead | existing | additionally issue the re-rooted horizon demands (one handle per root) | (unchanged) |
| RT callback | existing | pop the ring by position + epoch gate, crossfade, add to the root page | render (guard) |
| `MidiOutScheduler` | existing | drain the immediate thru ring first | — |
| Main | existing | build/swap plans, arm/disarm, recorders, place-* actions, meters | — |

`twRtThreadGuard` generalises to a per-thread `RenderPolicy {Any, Never}` with
`markRtThread` / `markLiveThread` behind the one `freezePage` check (the RT's
`assert()` behaviour is preserved; the live marker counts and returns silence).
Shutdown, each step order-independent: disarm all (pump stop + join; processors
handed back; recording finalised) → `MidiInput::setCallback(nullptr)` + close →
`stopOutput()` → recording join → existing `stopScan()` + `TwLog::shutdown()`.

## 5. `retireComponentNodes(set)` — semantics
Queued/ready nodes whose component ∈ set are dropped: their demands complete as
"not produced" (a consumer treats it like a miss: stale/silence); Running ones
are waited for (bounded by one page render); dedup entries removed so a later
demand plans fresh. Because the exclusion wiring precedes the drain, no NEW plan
contains the set; in-flight dependents of old plans see a miss. Non-graph
demanders (asset captures on the reval lane, previews) are not retired — the
`liveOwned` guard makes their arrival silent + counted (gated ≈ 0). `pause()` is
NOT a stand-in (it drains import-time analysis and hangs the UI).

## 6. Model, verbs, settings
- `STrack`: `trackInput`, `monitorMode`; `ArmedForRecording` inert on load.
- `SSettings`: `audio/recordingOffsetMs/<deviceName>`, `midi/inputOffsetMs/<port>`,
  `midi/inputPortId/<name>`; `SOpt`: record mode, count-in bars, punch, input
  quantise, retro buffer.
- Verbs: `set-track-input`, `set-monitor-mode`, `arm-track` (absolute, undoable);
  `record-start` / `record-stop` (transport-level); `place-midi-recording`,
  `add-midi-take`, `place-retro-midi` (later); `set-record-mode`, `set-count-in`.
- Testkit: `midi-in-event`, `midi-in-replay`, `assert-monitor-latency`
  (`inputFile`, `channel`, `maxFrames`; cross-correlation lag), `assert-audio-
  continuity` (`filename`, `startFrame`, `frameCount`, `maxGapFrames`,
  `maxStep`), `assert-render-policy` (`liveThreadRefusals`, `liveOwnedRefusals`
  bounds at exit), `assert-recorded-clip`, `assert-input-meter`, `virtual-key`
  `hold`/`release`/`durationMs`.

## 7. Invariants (new or amended) — as v3 §7 plus
playback: the two-lane machine and `out = frozen + ring`, ring entries stamped
and epoch-gated, capture cleared at device start; plugins: `twLiveTransport`
consulted only while `liveOwned`; mix: `isLiveOwned` is a wiring predicate, not
audibility; devices: immediate thru ring (second producer path), capture threads;
model: the placement conversion + `recordStart` definition; testkit: rule 1
amended, `--test-case` audio input default `null`.

## 8. Phases (summary — briefs with ACs in `21_ORCHESTRATION.md`)

| Phase | Deliverable | Depends on | Headline gates |
|---|---|---|---|
| **L0** input device layer + seams | capture threads + rings (WASAPI event-driven), `SMARAGD_AUDIO_INPUT_BACKEND=file/null`, paced `FileAudioInput`, `midi-in-event`/`-replay` over the existing capture input, `retireComponentNodes` (§5), `RenderPolicy` markers, settings keys | — | `devices_input_test`, `schedule_test`, guard test, goldens |
| **L1a** live lane engine | pump + stamped/epoch-gated ring + live clock (`twLiveTransport`) + speaker two-lane lifecycle + `setLiveOwned` guard + `playback_test` per transition + a synthetic-plan harness | L0 | `playback_test` transitions; harness renders a plan block-wise and matches the frozen render sample-exact where the identity holds |
| **L1b** live lane app: audio monitoring | plan builder + closure, exclusion wiring, re-rooted demands, `trackInput`/`monitorMode`/`arm-track`, meters, UI, render-suspends-live | L1a | `monitor_through_chain`, `monitor_latency` (cross-corr), `monitor_folder_closure`, `arm_during_playback` (continuity), goldens, sweeps |
| **L2** live instruments (37 P8a) | keyboard port, MIDI rings fan-out, `twLiveEventSource` as a feed member, ownership protocol, thru ring, `virtual-key hold/release`, Options MIDI inputs active | L1b | `live_instrument_play`, `live_instrument_merge`, `live_instrument_disarm_playback` (continuation after disarm ≡ no-arm capture), ownership counter, thru ≤ 2 ms |
| **L3a** capture bridge engine | `twGrowingCaptureSource`, `SRecordingContent`, three sinks + backpressure, wave writer consumer, `RecordingSession` refactor | L1a | `record_bridge_test` (sink identity vs pages) |
| **L3b** audio recording app | placement conversion + `recordStart`, non-modal, punch, loop takes, `locatorHeldElsewhere` retired, `startRecording` via `setPlaying`, offsets in Options | L1b, L3a | `record_offset_zero`, `record_loop_takes`, `record_punch`, existing take/placement cases |
| **L4** MIDI recording (37 P8b) | `SMidiRecorder`, `place-midi-recording`, `add-midi-take`, modes, loop takes, quantise, retro (opt.) | L2, L3b | `midi_record_placement`, `midi_record_modes`, `midi_record_loop_takes` |
| **L5** transport polish | metronome click source in the plan (pump exists iff a live lane is ON), count-in, pre-roll, latency readout, live-path plugin latency badge | L1b (L3b for count-in placement) | click at the grid; render byte-identical; count-in placement |
| **L6** *(outline)* multi-device duplex + drift, loopback wizard, ASIO validation (35) | 35 | own briefs |

Critical path: L0 → L1a → L1b → L2/L3a → L3b → L4 → L5.

## 9. Risks
| Risk | Mitigation |
|---|---|
| WASAPI shared latency makes monitoring feel bad | say so; ASIO (35) is the prerequisite; the design is correct at any block |
| Arm/disarm during playback clicks | epoch-gated flip + crossfade; gated by continuity assertions |
| A worker/preview touches a live processor | `setLiveOwned` guard + counter gated ≈ 0 |
| Live folder's unarmed siblings never frozen | re-rooted demands, one handle per root — a folder gate |
| A master chain appears later | D3 option (b), named |
| Placement sign/anchor error | D6's derivation + the zero-offset gate |
| Two clocks drift | L6; single-device duplex first |
| Sequenced material sounding while stopped | D2's feed gate, gated |

## 10. Decisions taken in v3/v3.1 (requester may veto before L1)
1. Input-only monitoring for an armed audio track (its own clips silent while
   armed; hearing them needs proposal 20 §2) — the excluded folder's own clips too.
2. Monitor Off/On/Auto per track; Auto = tape-machine style; On replaces.
3. Record modes new-take (default) / overdub / replace as a global setting.
4. The computer keyboard is a real input port, selectable per track.
5. Note-on chase ON at PLAY start for sequenced material; nothing sequenced
   sounds while stopped.
6. MIDI-thru follows `midiOutPort` when set, via the immediate ring.
7. Renders suspend live lanes (export ignores the split).

## 11. Deliberately not in scope
Direct/hardware monitoring; input FX; sends/sub-busses; a master chain; live
time-stretch; ASIO itself (35); cross-device drift (L6); MPE live routing
(works if the ABI passes it, not gated); hearing armed tracks' own clips (20 §2).

## 12. Glossary
| Smaragd | Cubase | REAPER | Logic | Studio One |
|---|---|---|---|---|
| live horizon / live-owned set | ASIO-Guard real-time + dependent channels | armed tracks (anticipative FX off) + receivers | live tracks (I/O buffer) | native low-latency monitor path |
| frozen lane | ASIO-Guard pre-processed | anticipative FX / media buffering | Process Buffer Range | Dropout Protection block |
| monitor Off/On/Auto | Manual / (On) / Tapemachine | off / on / auto | Software Monitoring + Auto Input | monitor button + tape-style |
| recording offset | Record Shift | manual offset + driver latency | Recording Delay | Record Offset |
| capture bridge | — | — | — | — |
| retrospective capture | Retrospective Record | retroactive record | Capture Recording | Retrospective Record |

## 13. Adversarial review (v3 → v3.1)

| # | Sev | Finding | Resolution |
|---|---|---|---|
| 1 | BLOCKER | No transport model: position/continuity/feed undefined; anchor exists only while playing; `ctx.playing` always true | **D2** (live clock: engine-published position + lead while playing, virtual counter + masked feed + held automation while stopped; `twLiveTransport` flag-gated in `render`; stamped ring; explicit repositions) |
| 2 | BLOCKER | MIDI-thru = second producer on the single-producer scheduler ring / 20 ms via the shell | D8: dedicated immediate SPSC ring on the scheduler, device-thread producer; ≤ 2 ms measured |
| 3 | BLOCKER | "Live-only output state" is a speaker lifecycle rewrite (new engine per start, zero-fill unless PLAYING, capture cleared per start) | D5: explicit device × frozen × live machine; `out = frozen + ring`; capture cleared at device start; testkit rule 1 amended; L1a phase with `playback_test` per transition |
| 4 | MAJOR | Case (iii) double-counts siblings; crossfade cannot know when the root stopped containing the track | D3: topmost member nulled, nested clip-muted, folder's own clips silent; epoch-gated flip (D2) |
| 5 | MAJOR | `syncInstrumentSlot` overwrites a processor-level live source; `setEventSource` bumps epoch | D4: live source is a MEMBER of `eventFeed()` while live-owned |
| 6 | MAJOR | Render while armed hits the guard → silent track | D4: renders suspend live lanes; L2 AC4 → concurrent non-root demand |
| 7 | MAJOR | Growing recording cut does not exist | D7: `twGrowingCaptureSource` + `SRecordingContent`, preview policy, macro replacement |
| 8 | MAJOR | Latency ACs below the position decoder's resolution | D9: cross-correlation `assert-monitor-latency`; budgets in frames measured sub-block |
| 9 | MAJOR | Verbs missing from deliverables | §6 + briefs list them |
| 10 | MAJOR | `retireComponentNodes` under-specified; `pause()` stand-in hangs | §5; stand-in deleted |
| 11 | MAJOR | L2 AC3 vacuous (render already barriers) | L2 gate: playback continuation after disarm ≡ no-arm capture |
| 12 | MAJOR | `recordStart` undefined; sign argument | D6 derivation + published-anchor definition |
| 13 | MAJOR | L1/L3 too big for one agent | L1a/L1b, L3a/L3b |
| 14 | MAJOR | `isLiveOwned` folded into `isLaneAudible` breaks feeds/meters | D3: separate wiring predicate |
| 15 | MAJOR | meters do not tick while stopped-and-armed | D9 |
| 16 | MAJOR | plan lifetime unlisted; `startRecording` bypasses `setPlaying` | §3 triggers; D7 |
| — | MINOR | existing capture MIDI input + env; not allocation-free; degraded fallback redundant; test-case input default; WASAPI not event-driven; RT assert vs marker; per-root demand handles; monitor Auto semantics; metronome vs "pump exists only when armed"; `kCacheEntries` text; RT vector allocs; L3 AC1 render; L5 count-in placement | all folded in (D1, D5, D9, L0/L5 briefs) |
