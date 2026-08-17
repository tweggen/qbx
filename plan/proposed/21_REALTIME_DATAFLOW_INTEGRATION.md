# Proposal 21 (DRAFT v3): Real-time data flows in the demand-driven dataflow

> **Status: DRAFT v3 (2026-08-17).** v3 supersedes v2 (2026-07-19), which
> predates proposal 36 (multichannel: wide pages, one wide chain per track,
> `calcOutputTo` deprecated and channel-clamped) and proposal 37 (instruments
> with a reposition protocol, MIDI I/O device layer, automation, event feed).
> **v2's central mechanism is dead:** "run the same component instances
> block-wise via `calcOutputTo`" cannot carry N channels, is on proposal 20's
> deletion list, and holds component mutexes across recursive pulls. What
> survives, one level lower, is the plugin *processor's* block renderer plus
> two pure functions (gain, channel map) — and that is enough. v3 keeps v2's
> concept (the **live horizon**), retires its mechanism, and adds what v2 left
> unnamed: folder parents as shared downstream, demand re-rooting at the
> horizon, honest latency numbers, the input device layer, MIDI recording, and
> a headless test story. Written from three concept studies (engine, app,
> industry) and one adversarial review (§13). Execution companion:
> `21_ORCHESTRATION.md`.
>
> Prerequisite reading: `19_ASYNC_FREEZE_MODEL.md` ("Phase 2 REVISED",
> execution classes), `20_DATAFLOW_FOLLOWUPS.md`, `36_MULTICHANNEL_SIGNAL_FLOW.md`
> §4, `37_MIDI_INSTRUMENTS_AUTOMATION.md` D4/D6/§3.2.1/§4.3/§4.4, `docs/contracts/
> {THREADING,FREEZE_PROTOCOL}.md`, `tw303a/{playback,schedule,plugins,devices,
> record}/CONTRACT.md`, `35_ASIO_BACKEND.md` (the hardware prerequisite for
> low-latency numbers — see §4).

---

## 0. Scope

| # | Capability | Industry anchor |
|---|---|---|
| A | **Live audio input** heard through the armed track's inserts → its parent folders → the master, on the same plugin instances playback uses, while the rest of the arrangement keeps playing from frozen pages | Cubase ASIO-Guard's "record-enabled/monitored channels and all dependent channels switch to real time"; REAPER anticipative FX disabled per armed track |
| B | **Live MIDI input** (hardware port or the computer keyboard) into the armed track's instrument, merged with its sequenced notes, MIDI-thru to the track's port | every reference DAW; Cubase VSTi record-enable → real-time path |
| C | **Audio recording** through one input pump: monitor ring + wide capture pages (waveform grows while recording) + WAV, placed at stop with proposal 17's takes; per-device recording offset; punch/loop | REAPER/Cubase/Logic/S1 |
| D | **MIDI recording** to a new/overdub/replace `SMidiCut` with takes per loop pass, timestamps → ticks, input quantise at commit; retrospective capture later | Cubase Retrospective Record, REAPER retroactive record |
| E | Transport support: monitor modes Off/On/Auto, count-in, metronome click on the live path, latency readout | universal |

Not in scope: sub-block / hardware direct monitoring; cross-device clock drift
(§8 P6 outline); live time-stretch of input; input FX chains ("record wet");
sends/sub-busses (they do not exist yet — when they do, they join the closure
by the same rule); a master insert chain (would move §2's exclusion to its
option (b) — named, not built); the ASIO backend itself (proposal 35).

---

## 1. What is true today (facts that shape the design)

**Engine**
- F1. **The block pull is mono, deprecated, and locks across recursion.**
  `twStreamingLatch::copyData` clamps to the latch index (graph inv. 12);
  `calcOutputTo` is on proposal 20 §3's deletion list; a plugin's legacy pull
  feeds channel 0 only and holds `pluginsMutex_`/`mutex()` across whole pulls;
  an instrument's legacy pull renders silence (plugins inv. 42). Nothing in
  the live lane may use it.
- F2. **What survives block-wise, wide:** `twPluginSlotProcessor::render(in,
  out, len, startPos, positional=true, sr)` — any block length (chunked to
  4096, short last chunk legal), state-continuous when `startPos == lastEnd_`,
  automation and the event feed applied per chunk (37 P3b/P5) — the exact call
  `twPluginInsert::renderCore_` makes minus the page gather; `twGainStage::
  applyGain(src, dst, n, pos, envelope)` (static, pure in position);
  `twRewire::channelMap()` (pure). Everything a live-owned track needs is one
  of these three.
- F3. **`renderPageWide(page, frames < FRAME_CAPACITY)` is accepted, but its
  input side is page-shaped**: every wide override calls
  `fetchInputPage(plug, page.startPosition)`, which on a miss freezes a FULL
  page at an unaligned start into the producer's cache. Partial page renders
  are usable only at page-aligned starts, i.e. useless block-wise. Small pages
  per lane are not viable either: `FRAME_CAPACITY` in 99 places / 28 files,
  grid alignment baked into `copyData`, the RT cursor, the readahead, the
  scheduler expansion, `planPage`, cache keys, `previousPage` chaining, meter
  probes.
- F4. **The master is a unity sum plus an identity map**: `SStdMixer` builds one
  `twMixer` and an identity `twRewire` root, no master chain, no sends. Post-B4
  a track is `twTrackMix → twPluginChain → twGainStage → twRewire`, all wide.
  `SStdMixer::reconnectTracksToMixer` already nulls the input plug of an
  inaudible track; `twComponent::planPage` skips a nulled plug (no nodes are
  planned for what hangs behind it); a nested lane's audibility is
  `twTrackMix::setClipMuted` (`applyChildTrackAudibility`).
- F5. **The RT callback reads root pages by position with the stale fallback**
  (proposal 16), pops N planar buffers (`pullBlock`, playback inv. 8), and
  `twSpeaker` maps them stereo onto the device (inv. 9). `startOutput` defers
  the device start until the readahead has primed ~3 s (`minBufferFrames_` =
  144000) and the callback zero-fills unless PLAYING.
- F6. **Latency numbers.** WASAPI shared: `kRequestedDurationHns` = 100 ms,
  variable callback block (`bufferFrames − padding`), `outputLatencyFrames`
  from `GetStreamLatency`; the capture backend is a fixed 1024-frame block. v2
  §4's "8 ms" is not reachable on this backend; the honest budget is ~100 ms
  out + input until proposal 35 (ASIO) lands.
- F7. **Input side.** `AudioInput::read` is a non-blocking poll (`RecordingSession`
  spins with a 1 ms sleep); `WASAPIInput::read` releases a whole packet after
  copying only `framesToCopy` — it drops the tail of any packet larger than the
  caller's buffer (latent: the recorder asks 1024). `createAudioInput()` is a
  compile-time platform pick: no env selection, no file/capture input exists.
  `MidiInput` (37 P7a) delivers on a device thread with host-time stamps into
  a callback; `CaptureMidiInput::inject()` exists.
- F8. **Instruments (37 P3b/P3c)**: every non-contiguous page repositions
  (`reset` + chase + pre-roll ≥ 4096); `forgetContinuity()`; the render barrier
  `beginRun` walks instrument tracks at render/play start; every generator page
  is a pure function of position + feed (plugins inv. 40); the processor's
  event source is settable (`setEventSource`, which clears `haveLastEnd_`);
  `twEventMerge` namespaces note ids per source (16 sources).
- F9. **Meters read pages by position** (proposal 34); a live-owned track's
  root pages are not frozen while live, so its meter goes dark unless the pump
  publishes levels. `SMidiOutPump` (37 P7b) established the timing discipline:
  anchor on position PUBLICATION (`locatorPublishSeq` + `hostNowNs`), the
  published position is one device buffer ahead of the frame just delivered,
  `meterLatencyFrames()` converts device→project frames.
- F10. **Threading**: `twRtThreadGuard` is a per-thread "never render" marker
  checked in `freezePage`; `CaptureRevalidator` has `pause()/resume()` (drains
  ALL in-flight jobs) and `retireObject` (IRevalidatable only) — no per-
  component node retirement; teardown is `smaragdOrderlyShutdown` (immortal
  `TwLog`, `stopScan()`), `MidiOutScheduler` joins Qt-free.

**App**
- A1. Recording today: `startRecording` starts the record worker FIRST so
  `locatorHeldElsewhere()` makes it the playhead authority; "monitoring" is
  frozen playback of the arrangement; a modal progress dialog; `onRecordingCompleted`
  shifts the placed clip by `outputLatency − inputLatency` in DEVICE frames (no
  rate conversion) and calls `place-recording` (takes for covered columns, one
  composite). No count-in (metronome is a stub prop), no punch, no loop record.
- A2. `SAutomationRecorder` (37 P6) is the model for a transport-bounded
  recorder that commits ONE action at stop; `SVirtualKeyboardDock` (37 P4)
  submits `add-note` at the locator with no release; `SAddTakeAction` is audio-
  only (`filePath`/`SCut`), `STakeStack::insertTake` already enforces a
  homogeneous kind.
- A3. Per-track attributes so far: `midiOutPort/Channel/OffsetMs`,
  `midiRouting`, `ArmedForRecording` (serialized — must not trigger monitoring
  on load). Options: audio input combo is a "System default" TODO; the MIDI
  page lists inputs but they are inactive.

---

## 2. The eight decisions

### D1 — The live lane is a *live executor* over processors, not a second component graph
A `LiveGraphPump` (one std::thread per active live lane) executes a **live plan**
— an immutable, main-thread-built snapshot of the live-owned subgraph — once
per device block: for each armed track, `input ring (or instrument events)` →
`processor->render(…, positional=true)` for every slot in slot order → `applyGain`
→ `channelMap`; for a folder parent in the closure, sum its live children with
its unarmed children's frozen root pages read **by position** (`getPageIfExists`
try-lock; miss = silence for that input this block, keep the previous page like
`twLevelProbe`) → the parent's own processors/gain/map; the top of the closure
is pushed into an SPSC `liveMixRing_` (planar N × block, 3–4 deep). Rejected:
(a) a wide block-pull virtual on every component — the B1–B4 sweep again plus
the lock recursion that made the pull unsuitable for a near-RT thread; (b)
small pages per lane (F3); (d) v2's degraded strip — kept only as the FALLBACK
for a chain the pump cannot host (a mapping-`Unsupported` slot or, when the ABI
grows RT-fitness metadata, an unfit plugin), with a UI badge.

### D2 — Exclusion by the existing wiring rule; RT = frozen root page + live ring; exact by linearity
An armed-and-monitoring track is "inaudible to the frozen sum" by exactly the
rule mute/solo use: `reconnectTracksToMixer` nulls its plug (a nested lane:
`setClipMuted`), the mixer/root epoch bumps, the master's pages re-sum over the
remaining track roots (milliseconds per page; track pages untouched), and
`planPage` skips the nulled plug — **no node is ever planned for the live-owned
chain**, so exclusive ownership of its processors is by construction for new
demands (in-flight ones are drained, D3). The RT callback keeps reading root
pages exactly as today and adds the ring: `root(unarmed) + live(armed) ==
root(all)` sample-for-sample because the master is a unity sum with an
identity map (F4). The flip is **crossfaded in the RT mixer** (2–3 ms, RT-safe)
so arm/disarm during playback is click-free rather than a one-block hole.
Rejected for now: the pump rendering the whole master from N frozen track
roots (v2's shape) — zero invalidation on arm but duplicates the RT's stale-
fallback logic for N roots, darkens the master meter, and buys nothing while the
master is linear; it becomes the design the day a master insert chain exists.

**Two corrections to v2:** (i) v2's `liveExcluded_` inside `SStdMixer::freezePage`
does not exist and cannot — `SStdMixer` has no freeze override; exclusion is
wiring. (ii) v2 §7's "demands stop at the horizon" is wrong: under exclusion the
frozen siblings beneath a live folder are unreachable from the root demand, so
**the readahead thread re-roots demands** at every "frozen input of the live
plan" (`requestGraphPages(siblingRoot, pos, n, prio 9)`), from a list handed
over with the plan. The pump itself never demands (planner locks).

### D3 — Instrument live mode is P3b's processor driven contiguously; ownership is a protocol
No `twLiveInstrument`. Arm (main thread): build the plan → apply the exclusion
wiring → **drain** in-flight nodes holding the chain's processors
(`CaptureRevalidator::retireComponentNodes(set)`; `pause()/resume()` is the P1
stand-in) → `slot->forgetContinuity()` → `proc->setLiveOwned(true)` (assert-
first: a freeze-path `render` arriving while live-owned answers silence and
counts, so a missed drain is measurable, never a corrupted voice) → swap the
processor's event source to `twEventMerge{trackFeed, twLiveEventSource}` BEFORE
the first block (`setEventSource` clears continuity). The first live block is
one reposition (reset + chase + pre-roll from the FEED at the arm position — the
sequenced held notes come up correctly, D4 of 37 for free); every later block is
contiguous. `twLiveEventSource::collect` drains the MIDI ring, maps host time →
project frame with the pump's anchor (minus input latency), rebases, **clamps
late events to offset 0 (never drops)**, and keeps its own held-note table for
the one chase at live start. Disarm: last block flushed with all-notes-off →
restore the plain feed → `forgetContinuity()` → re-wire into the frozen sum →
`invalidateRenderPathRange(armPos, ∞)` (FREEZE_PROTOCOL's order). The recorded
clip is by then part of the feed, so the frozen re-render of `[armPos, ∞)`
reproduces the take deterministically (plugins inv. 40). `beginRun` (37 P3c)
**skips live-owned tracks** — a `forgetContinuity()` on a sounding live
instrument would cut its voices.

### D4 — Latency honesty; the device runs live-only; the input gets a real capture thread
WASAPI shared gives ~100 ms out; **proposal 35 (ASIO) is the hardware
prerequisite for software-monitoring quality** — 21 is correct at any block size,
only slow, and every PR body says which. The callback gains a **live-only
state** (frozen lane idle, ring popped, device running with the transport
stopped — monitoring while stopped is the normal case) and the readahead's
priming must not gate the ring. The input side gets an **event-driven capture
thread per device** writing an SPSC ring (fixing F7's packet-tail drop as a side
effect); the pump pops. Round trip = input period + ring hop + pump lookahead
(1–2 blocks) + output buffer + reported latencies. **One named conversion for
placement**: capture frame k lands at `recordStart + k − inputLatencyProj −
outputLatencyProj + userOffsetProj`, both latencies in PROJECT frames through the
same scaling as `meterLatencyFrames()`; it shares the ANCHOR with 37 P7's MIDI-out
(`locatorPublishSeq` + `hostNowNs`) but is an independent knob (`recordingOffsetMs`
per input device, in `SSettings`).

### D5 — One input pump, three sinks; recording is a consumer of the bridge
Per active input: (1) the live-lane ring, (2) **wide** capture pages
(`twCapturingSource` planar ctor — proposal 36 trap 27) published as a growing
"recording cut" behind an atomic `captureFrontier_` (the waveform grows while
recording; post-stop playback serves from cache), (3) the WAV writer.
`RecordingSession` becomes a ring consumer (no more 1 ms polling; non-modal —
the dialog polls). `place-recording` stays the endpoint at stop, unchanged
(takes for covered columns; loop record = one take per pass via the pump's
wrap detection + `add-take startOffset=` — proposal 17's "phase 5" falls out).
`locatorHeldElsewhere()` retires: the OUTPUT publication is the playhead
authority in every mode.

### D6 — MIDI recording is a main-thread recorder committing one action per pass
`SMidiRecorder` mirrors `SAutomationRecorder`: bounded by the transport
(`transportStarted/Stopped`), reads the same event ring the live lane reads (a
tee — the recorder never touches the pump), stamps host time → project frame
(shared `SPlayheadClock`, extracted from `SMidiOutPump`'s anchor logic) → ticks
(tempo map, once), and at stop commits **`place-midi-recording`** (planner shape
of `place-recording`: a new take where a MIDI clip already covers the column —
new verb `add-midi-take`, since `add-take` is audio-only — else `insert-midi-clip`
with events); modes **new take / overdub (merge into the covering clip) /
replace** as a global setting; loop record = one take per pass in one macro;
input quantise = a second `quantize-notes` action inside the macro; retrospective
capture (a rolling ring turned into a clip after the fact) as a later option
(`place-retro-midi`). All-notes-off + CC/sustain chase on stop/locate (the P7
pump's rules).

### D7 — One per-track input selector; monitor Off/On/Auto; the computer keyboard is a real port
`STrack::trackInput = none | audio:<device>:<channelMask> | midi:<port>:<channel|any>
| keyboard` (portable NAMES via `SSettings`, like `midiOutPort`; default derived
from `instrumentSlot()` for the head's right-click menu), `monitorMode = auto |
on | off` (auto = armed && (stopped || recording), REAPER/Cubase semantics),
serialized only when non-default; arm reuses `ArmedForRecording` — the KIND of
what arming captures is the input's kind, not a second flag. The live set =
closure of `{armed && monitorEffective}` ∪ `{monitor == on}` up to the master.
`SVirtualKeyboardDock` becomes an in-process `MidiInput` port ("Computer
keyboard": press/release = note-on/off) so live play and recording treat it like
hardware; step-input at the locator remains the stopped-transport behaviour.
MIDI-thru: the live event source tees to `MidiOutScheduler` at `dueNs = now` when
the track has a `midiOutPort`. Input meter (pre-FX): a lock-free peak pair
published by the bridge, read by `pumpMeters`; the post-FX meter of a live track
is published by the pump position-keyed (proposal 34's tap goes dark otherwise).

### D8 — Testable headlessly, or it is not built
`SMARAGD_AUDIO_INPUT_BACKEND = file:<wav> | null | default` (mirror of the
output knob): `FileAudioInput` replays a WAV **paced on the same steady clock**
the capture backend and `MidiOutScheduler` use, with a configurable reported
input latency; `SMARAGD_MIDI_INPUT_BACKEND = capture` (the existing
`CaptureMidiInput`) fed by new verbs `midi-in-event` (inject now / at a frame)
and `midi-in-replay` (a `.mid` or text log with host-time stamps). Because the
audio capture backend already logs `{hostTimeNs, firstFrame}` per delivered
block, the gates are **independent of the lane under test**: monitoring latency
= position-encoded input WAV → `dump-playback-capture` → `assert-source-position`
(± one block, deterministic given paced clocks); "through the chain" = the test
CLAP gain at 0.5 halves the captured RMS; recording offset = the placed clip's
`assert-source-position` must read 0 ± 1 block after compensation; MIDI-in
placement = the recorded note's tick vs its injected host time through
`frameAtHostTime`. **Goldens byte-identical whenever nothing is armed** holds by
construction: the pump does not exist unless something is armed and exclusion is
the wiring rule the frozen lane already obeys. NOT measurable: real driver
latency, hardware jitter, WASAPI shared under load — every PR body says so.

---

## 3. The live horizon, precisely (post-36 graph)

Live-owned set, per case (the closure = the set ∪ every summing ancestor up to,
but not including, the root, plus the master crossfade in the RT):

| Case | Live-owned | Frozen inputs read by position | Notes |
|---|---|---|---|
| (i) armed AUDIO track | its slot processors (all slots), gain stage, channel map; source = input ring | none for P1 (input-only monitoring; hearing its own clips live needs proposal 20 §2 caching because `twTrackMix` caches nothing) | the track's clips keep playing from the frozen lane? NO — an excluded track's clips are silent while armed unless the pump also plays them (P4 option, after 20 §2) |
| (ii) armed INSTRUMENT track | slot-0 processor with `events = merge{feed, liveRing}`, later slots, gain, map | none | its own audio clips: same caveat as (i) |
| (iii) folder parent of a live child | the parent's SUM (live children + unarmed children's frozen root pages by position) + its processors + gain + map | every unarmed child ROOT (re-rooted demands, D2) | the only genuinely shared downstream today; child MIDI bubbling to a parent instrument makes the parent the live INSTRUMENT (ii) and the child a MIDI source only |
| (iv) sends / sub-busses | — | — | do not exist; when they do: closure by the same rule (Cubase "dependent channels") |
| root | not live: `root(unarmed) + ring` in the RT | the root page itself | exact by linearity; a future master chain moves this to D2's option (b) |

Arm/disarm are edit-path operations on the main thread with an audible budget
like a seek; during playback the RT crossfades. `beginRun` skips live-owned
tracks; disarm applies the barrier to the returning track.

---

## 4. Threading contract

| Thread | New/changed | May | Must not |
|---|---|---|---|
| Input capture (per device) | NEW, device-owned, event-driven | write its SPSC ring | touch Qt, block |
| MIDI input (device) | existing (P7a) | write the event ring (+ tee for the recorder/thru) | touch Qt |
| **`LiveGraphPump`** | NEW std::thread, MMCSS "Pro Audio", below the device thread, above the readahead | take a processor's `mutex_` (bounded; the freeze path never takes it once live-owned), `getPageIfExists` try-lock, push the live ring, publish levels/atomics | `freezePage`/`requestPage`/`fetchInputPage`/`copyData` (render on miss), `requestGraphPages`, any blocking component `mutex()`, `queueLock_`, allocation (plan is an immutable swapped snapshot with pre-sized scratch), Qt |
| Readahead | existing | additionally issue the re-rooted horizon demands | (unchanged) |
| RT callback | existing | pop the ring, crossfade, add to the root page | render (guard) |
| Main | existing | build/swap plans, arm/disarm, recorders, place-* actions, meters | — |

`twRtThreadGuard` generalises to a per-thread `RenderPolicy {Any, Never}` with
two markers (`markRtThread`, `markLiveThread`) behind the one check in
`freezePage`. Shutdown (`smaragdOrderlyShutdown` / `~SApplication`), each step
order-independent (rule 4): disarm all (pump stop + join; processors handed back;
recording finalised) → `MidiInput::setCallback(nullptr)` + close → `stopOutput()`
→ recording join → existing `stopScan()` + `TwLog::shutdown()`.

---

## 5. Model, verbs, settings

- `STrack`: `trackInput`, `monitorMode` (serialized when non-default; older
  builds ignore); `ArmedForRecording` never starts monitoring on load.
- `SSettings`: `audio/recordingOffsetMs/<device>`, `midi/inputOffsetMs/<port>`,
  `midi/inputPortId/<name>`; `SOpt`: record mode (newtake/overdub/replace),
  count-in bars, punch enabled, input quantise grid/off, retrospective buffer.
- Verbs (all ABSOLUTE, undoable where they mutate the model): `set-track-input`
  (`trackPath`, `input`), `set-monitor-mode` (`trackPath`, `mode`), `arm-track`
  (`trackPath`, `armed`), `record-start` / `record-stop` (transport-level, not
  undoable; `toggle-record` for the UI), `place-midi-recording` (`trackPath`,
  `startTime`, inline `<e …/>` events or `filePath=…mid`, `mode`), `add-midi-take`
  (`clip`, events, `index`, `activate`), `place-retro-midi` (later),
  `set-record-mode` (setting), `set-count-in`.
- Testkit: `midi-in-event` (`bytes` or `kind/key/velocity/channel`, `atFrame`
  or now), `midi-in-replay` (`filePath`, `startFrame`), `assert-recorded-clip`
  (`trackPath`, `index`, `startTime` ± tol, `duration` ± tol, `kind`,
  `takeCount`), `assert-monitor-latency` (`maxFrames`, via the position-encoded
  fixture + `dump-playback-capture`), `assert-input-meter`, `virtual-key`
  gains `hold`/`release`/`durationMs`.

---

## 6. Capture bridge details
Wide capture through the planar `twCapturingSource` ctor; the recording cut is
an ordinary `SCut` over a growing content whose frontier advances as pages are
published (the arranger draws it; `place-recording` finalises it as today).
Timeline stamping: frame 0 of the capture = `recordStart − inputLatencyProj −
outputLatencyProj + userOffsetProj` (D4). WAV: the existing writer, fed by the
bridge, one file per input device (multi-input = multiple bridges). MIDI: the
recorder's table → ticks via `twTempoMap` → `SMidiSequence` at stop (or at each
loop wrap for a take). `RecordingParams.channels`/`recordingChannels_` are not
truth — the device's channel count is (36 trap 2).

---

## 7. Invariants (new or amended)

| Contract | Amendment |
|---|---|
| THREADING | inventory rows: input capture thread, `LiveGraphPump`; rule 6: "the pump never renders, demands, or blocks on a component mutex; a live-owned processor is rendered by the pump alone"; `RenderPolicy` markers |
| FREEZE_PROTOCOL | "Live-owned components: no node is planned for them (exclusion wiring); on return the barrier applies (`forgetContinuity` then `invalidateRenderPathRange(armPos, ∞)`)" |
| playback/CONTRACT | live-only output state; RT = root page + ring with a crossfade; `locatorHeldElsewhere` retired (output publication is the authority in every mode) |
| schedule/CONTRACT | `retireComponentNodes`; horizon demands are issued by the readahead, never by the pump |
| plugins/CONTRACT | `setLiveOwned` guard; live mode = contiguous `render`; event source swap only between arms; `beginRun` skips live-owned |
| mix/CONTRACT | exclusion = the audibility wiring rule; the master identity `root(unarmed) + live == root(all)` |
| devices/CONTRACT | input capture thread + ring; `SMARAGD_AUDIO_INPUT_BACKEND`; `FileAudioInput` paced on the shared clock; `CaptureMidiInput` inject/replay |
| record/CONTRACT | `RecordingSession` is a bridge consumer; the one placement conversion |
| track/CONTRACT | `trackInput`, `monitorMode`; arm kind = input kind |
| model/CONTRACT | placement conversion named once; recorders commit one action per pass |
| testkit/CONTRACT | the paced file input; independence of latency measurement; new verbs |
| CLAUDE.md | "Recording Audio" rewritten around the bridge; the knobs |

---

## 8. Phases (summary — briefs with ACs in `21_ORCHESTRATION.md`)

| Phase | Deliverable | Depends on | Headline gates |
|---|---|---|---|
| **L0** Input device layer + test backends | event-driven capture threads + SPSC rings (WASAPI; CoreAudio/ALSA guarded), `SMARAGD_AUDIO_INPUT_BACKEND=file/null`, `FileAudioInput` paced, `CaptureMidiInput` inject/replay, `midi-in-event`/`midi-in-replay`, per-device offsets in settings, `RenderPolicy` markers, `retireComponentNodes` | — | `devices_input_test` (ring, pacing, no packet-tail loss vs the old poll), `schedule_test` retirement, goldens by construction |
| **L1** Live lane: audio monitoring through the chain | live plan + closure, exclusion wiring, re-rooted demands, `LiveGraphPump`, live-only output state, RT ring + crossfade, `trackInput`/`monitorMode`/`arm-track`, input meter, live-track meter publication, degraded fallback | L0 | `monitor_through_chain` (file input → chain gain 0.5 → capture RMS), `monitor_latency` (position-encoded, ± 1 block), `arm_during_playback` (no discontinuity above threshold), goldens byte-identical unarmed, `repeat_test` sweeps |
| **L2** Live instruments (MIDI in) = 37 P8a | keyboard as an in-process port, `MidiInput` device port → ring → `twLiveEventSource`, ownership protocol, note chase at live start, MIDI-thru, `virtual-key hold/release`, `beginRun` skip | L0, L1 | `live_instrument_play` (injected note audible ± budget, sequenced + live both sound), `live_instrument_disarm_rerender` (deterministic re-render after disarm), thru to the capture MIDI port |
| **L3** Capture bridge + audio recording | wide capture pages behind a frontier, waveform-while-recording, WAV consumer, `RecordingSession` refactor, non-modal, the placement conversion, punch region, loop takes for audio, `locatorHeldElsewhere` retired | L1 | `record_offset_zero` (position-encoded, 0 ± 1 block after compensation), `record_loop_takes` (2 passes → take stack of 2), `record_punch`, existing `takes_recording_placement` green |
| **L4** MIDI recording = 37 P8b | `SMidiRecorder`, `place-midi-recording`, `add-midi-take`, modes, loop takes, input quantise, `assert-recorded-clip`, retrospective capture (optional) | L2, L3 | `midi_record_placement` (replayed note lands at its tick ± 4096), `midi_record_modes`, `midi_record_loop_takes`, one undo per pass |
| **L5** Transport polish | metronome click on the live path (renders untouched), count-in, pre-roll, latency readout, live-path plugin budget warning | L1 | click audible only on the device path (render byte-identical), count-in offsets the placement correctly |
| **L6** *(outline)* multi-device duplex + drift, loopback calibration wizard, ASIO validation (35) | 35 | own briefs |

Critical path: L0 → L1 → L2/L3 → L4 → L5.

---

## 9. Risks

| Risk | Mitigation |
|---|---|
| WASAPI shared latency makes monitoring feel bad | Say so; ASIO (35) is the prerequisite for the numbers; the design is correct at any block |
| Arm/disarm during playback clicks | RT crossfade; budgeted like a seek; gated by a discontinuity threshold on the capture |
| Live-owned instrument's meter goes dark | pump publishes position-keyed levels |
| A missed drain lets a worker touch a live processor | `setLiveOwned` assert-first guard + counter, gated |
| Live folder's unarmed siblings never frozen | re-rooted demands from the readahead (D2 ii) — a gate with a folder |
| Master gains an insert chain later | switch to D2 option (b); named |
| Recorder placement sign error (A1's device-frame shift) | the one named conversion + the zero-offset gate |
| Two clocks (input/output devices) drift | P6; single-device duplex first |

## 10. Decisions taken in v3 (requester may veto before L1)
1. Input-only monitoring for an armed audio track in L1 (its own clips are
   silent while armed) — hearing them live needs proposal 20 §2; scheduled as
   an L3/L4 option, not a blocker.
2. Monitor modes Off/On/Auto per track, Auto default (REAPER/Cubase).
3. Record modes new-take (default) / overdub / replace as a global setting.
4. The computer keyboard is a real input port, selectable per track.
5. Note-on chase ON at live start for the sequenced material (37 §11.2).
6. MIDI-thru follows `midiOutPort` when set, off otherwise.

## 11. Deliberately not in scope
Direct/hardware monitoring; input FX ("record wet"); sends/sub-busses; a master
chain; live time-stretch; ASIO itself (35); cross-device drift (L6); MPE live
expression routing (works if the ABI passes it, not gated).

## 12. Glossary
| Smaragd | Cubase | REAPER | Logic | Studio One |
|---|---|---|---|---|
| live horizon / live-owned set | ASIO-Guard real-time channels + dependent channels | armed tracks (anticipative FX disabled) + receivers | live tracks (I/O buffer) | native low-latency monitor path |
| frozen lane | ASIO-Guard pre-processed | anticipative FX / media buffering | Process Buffer Range | Dropout Protection block |
| monitor Off/On/Auto | Auto Monitoring modes | record monitoring off/on/auto | Software Monitoring + Auto Input | monitor button + tape-style |
| recording offset | Record Shift | manual offset + driver latency | Recording Delay | Record Offset |
| capture bridge | — | — | — | — |
| retrospective capture | Retrospective Record | retroactive record | Capture Recording | Retrospective Record |

## 13. Adversarial review
*(filled in by the review pass — findings, and what changed in response.)*
