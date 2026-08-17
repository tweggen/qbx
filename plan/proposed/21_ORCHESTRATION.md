# Proposal 21 — Orchestration plan (execution companion)

> **Status: PLAN v1.2 (2026-08-17)** for `21_REALTIME_DATAFLOW_INTEGRATION.md`
> v3.2. One Opus 5 sub-agent per phase (L0, L1a, L1b, L2, L3a, L3b, L4, L5),
> each in its own worktree/branch, closed only when every acceptance criterion in
> its GATE is green; the orchestrator reviews the *orchestrator-reviewed* items
> line by line, runs the gate verdict and merges via PR. Ground rules and loop as
> `37_ORCHESTRATION.md` §0–§2, restated briefly because this proposal touches
> the RT path.

## 0. Ground rules
1. **The proposal is the spec** (D1–D9, §10). No re-litigating "no block pull /
   no small pages / exclusion by wiring / RT = root + ring / live clock / one
   action per pass". A genuine contradiction in code → STOP, write it into §13,
   surface it.
2. **Gates are hard.** Never widen a latency budget or a band without a
   physically-grounded justification in the test comment and STATE.md; never
   touch a golden; never mark a flaky test expected-fail; **any RT/pump render-
   policy violation is a failure** (`assert-render-policy` at exit).
3. **Repo laws**: `./build.sh` (re-configure), `check_layering.py`,
   `check_logging.py`, `ctest -j4` from the repo root with the count reconciled;
   `repeat_test.sh` N ≥ 50 × `SMARAGD_REVAL_WORKERS` {1,4,8,16} for anything
   touching the scheduler, the readahead, the pump, the barrier or a class-1
   processor; no Qt off the main thread; POD thread_locals only; RT/pump never
   render; race fixes order-independent; playback/latency cases `RUN_SERIAL` at
   `SMARAGD_CAPTURE_SPEED=1`.
4. One phase per branch (`feat/21-l<N>-<slug>`), one PR; STATE.md entry with
   numbers and what was NOT gated (real hardware latency/jitter never are); tick §6.
5. Iterate until green; escalate after three genuinely different diagnoses, or
   for a hardware/ears judgment.
6. A task names its module set; touching another escalates.

## 1. Agent policy
Opus 5 implements each phase end to end. The orchestrator reviews line by line:
the RT callback delta and ring, the pump loop and live clock, the speaker
lifecycle, the exclusion wiring + drain, the placement conversion, the
recorders' commit. Recon/test authoring may fan out (read-only or disjoint new
files); implement OR verify, never both on one artifact.

## 2. The phase loop
ORIENT → PLAN → BUILD → GATE (every AC + standing gate; reconcile counts) → FIX →
CLOSE (STATE.md, tracker, PR with gated / NOT gated) → stop.

## 3. Phase briefs

### 3.0 Standing gate
```
./build.sh
python tools/check_layering.py
python tools/check_logging.py
ctest --test-dir smaragd/build -j4 --output-on-failure   # reconcile ctest -N
```
plus **byte-identical `smaragd/tests/goldens/`** (no live lane ⇒ the pump does
not exist; a golden that moves is a design violation, never a re-freeze) and
`repeat_test.sh` sweeps on every new playback case. Dependency graph:
```
L0 ─► L1a ─┬─► L1b ─┬─► L2 ─────────┐
           │        └─────────► L3b ─┴─► L4 ─► L5
           └─► L3a ─────────────┘
```
(L3a needs only L1a and runs in parallel with L1b; L3b needs L1b + L3a; L2 and L3b are independent.)

---

### L0 — Input device layer + two engine seams  *(tw/devices, tw/schedule, tw/graph, testkit, settings)*
- **Entry:** none.
- **Modules:** `tw303a/devices` (+CONTRACT), `tw303a/schedule` (+CONTRACT),
  `tw303a/graph` (guard), `main/shell/src/main.cpp` (env default), `main/testkit`,
  `SSettings` keys, `docs/contracts/THREADING.md`.
- **Deliverables:**
  1. `AudioInput` gains an **event-driven capture thread + SPSC ring** per open
     device: WASAPI re-initialised with its own capture event
     (`AUDCLNT_STREAMFLAGS_EVENTCALLBACK`, `SetEventHandle`, `GetBuffer`/
     `ReleaseBuffer` per packet — the WHOLE packet goes into the ring, fixing the
     tail drop); CoreAudio/ALSA written and guarded (unverified on Windows — say
     so); `read()` becomes a ring pop for existing callers.
  2. `createAudioInput()` honours `SMARAGD_AUDIO_INPUT_BACKEND = file:<wav> |
     null | default`; `--test-case` DEFAULTS to `null` (set in `main.cpp` next to
     the audio/MIDI defaults unless already set); `FileAudioInput` replays a WAV
     in 1024-frame blocks **paced on `MidiOutScheduler::hostNowNs()`** using the
     high-resolution waitable timer the scheduler already uses, configurable
     reported `inputLatencyFrames`, loop/stop-at-end option, own capture thread
     + ring like a device; `NullAudioInput`.
  3. Testkit verbs over the EXISTING `CaptureMidiInput::inject()` (selected by
     `SMARAGD_MIDI_BACKEND=capture`, already the `--test-case` default):
     `midi-in-event` (`kind`, `key`, `velocity`, `channel` or `bytes`; `atFrame`
     optional and valid only while PLAYING — mapped through the engine's
     delivered-frame atomic — otherwise `now`) and `midi-in-replay`
     (`filePath` .mid or text log, `startFrame`, real-time paced).
  4. `CaptureRevalidator::retireComponentNodes(span<const twComponent*>)` with
     the semantics of design §5 (queued/ready dropped + demands complete as
     "not produced", Running waited for, dedup entries removed); `schedule_test`.
  5. `twRtThreadGuard` → per-thread `RenderPolicy {Any, Never}` with
     `markRtThread()` (keeps today's `assert()`) and `markLiveThread()` (silence +
     `liveThreadRefusals` counter + one log); one check in `freezePage`.
  6. `SSettings` keys `audio/recordingOffsetMs/<deviceName>`, `midi/inputOffsetMs/<port>`.
  (No existing qxa case records audio — there is no record verb yet — so the `null`
  input default under `--test-case` changes nothing; say so in the testkit CONTRACT.)
- **Gate (ACs):**
  - AC1 `devices_input_test`: (a) `FileAudioInput` over a 2 s position-encoded
    WAV delivers every frame exactly once, in order (compare to the file), block
    delivery times within ±2 ms of the paced schedule on an idle box (RUN_SERIAL;
    the number recorded); (b) a synthetic packet 3× the pop size loses no frames
    through the ring; (c) injected MIDI events come out in order with stamps;
    (d) env selection incl. the `null` default under `--test-case`.
  - AC2 `schedule_test`: nodes of a retired component never execute after
    `retireComponentNodes` returns; a Running one is waited for; other nodes
    unaffected; a re-demand after retirement plans fresh; 100 randomized
    interleavings.
  - AC3 graph unit test: a `markLiveThread` thread calling `freezePage` gets
    silence, `liveThreadRefusals == 1`, one log; the RT marker's assert path
    unchanged (existing tests green).
  - AC4 `ctest -j4` 100 %; goldens by construction.
- **Orchestrator-reviewed:** the WASAPI capture thread; the retirement semantics; the guard.

### L1a — Live lane ENGINE: pump, ring, live clock, speaker lifecycle  *(tw/playback, tw/plugins, tw/mix identity note)*
- **Entry:** L0 merged.
- **Modules:** `tw303a/playback` (+CONTRACT; the ENGINE-owned position atomic
  `{seq, deliveredFrame, hostNs}` stamped in `twSpeaker`'s callback beside
  `publishPosition`), `tw303a/plugins` (`setLiveOwned`, `setLiveEventSource`,
  `twLiveTransport` — all flag-gated in `render`; +CONTRACT), `tw303a/mix`
  (CONTRACT identity note), `main/testkit` (testkit CONTRACT rule 1 amended:
  frame 0 = device session; existing `dump-playback-capture` cases unchanged
  because with the live lane OFF the device still opens at play), `playback_test`,
  docs/contracts.
- **Deliverables:** design D1/D2/D5 (engine half): `twLivePlan` (immutable:
  ordered live-owned tracks → per-slot processor pointers, gain envelopes, channel
  maps; folder sums with the frozen-input root list; scratch; `feedEnabled`,
  `holdAutomationAt`); `LiveGraphPump` thread (`markLiveThread`, MMCSS,
  allocation-free steady state) rendering per block per D1/D2 with the live
  clock (playing: `enginePublished + lead`; stopped: virtual counter), one explicit
  reposition on start/stop/seek/wrap; **position-stamped, epoch-tagged**
  `liveMixRing_`; the RT sums a ring entry only on position match AND
  `rootPage.contentEpoch >= flipEpoch`, 2–3 ms crossfade, mismatch/miss
  counters; **speaker lifecycle** device {CLOSED, OPEN} × frozen {IDLE,
  BUFFERING, PLAYING} × live {OFF, ON}, `out = frozen + ring`, `startOutput()`
  attaches the frozen lane to an open device, `stopOutput()` stops the lane
  only while live is ON, `openLive()/closeLive()`; the plan builder's **master-
  shape precondition** (`twMixer(unity) → twRewire(identity)` ⇒ the linear split
  `root(unarmed) + ring`; anything else ⇒ the master joins the closure and the RT
  pops the ring only — a unit test flips the shape and sees the mode change);
  capture backend cleared at
  DEVICE start; `twProcessContext.playing` truthful; the processor's SECOND
  event source `liveEvents_` (`setLiveEventSource`) collected alongside `events_`
  with namespaced note ids, `feedEnabled=false` skipping `events_` only,
  automation hold via the per-chunk chase build (no `setParamCurves` change);
  `flipEpoch`/`flipEpoch'` = the root rewire's `contentEpochNow()`; `setLiveOwned`
  guard + `liveOwnedRefusals`; a **synthetic-plan harness** in `playback_test` (a plan
  over test processors driven block-wise, compared to the frozen render of the
  same material where the identity holds).
- **Gate (ACs):**
  - AC1 `playback_test` transitions: CLOSED→OPEN(live) → PLAY attaches (no re-open,
    engine swap under the leaf lock) → STOP keeps the device → disarm closes;
    PLAY without live still opens/closes as today (existing tests green); capture
    frame 0 = device start.
  - AC2 harness (SYNCHRONOUS — no pacing; the input is a clip at frame 0 for
    the frozen render): a plan with `tw.test.clap.gain` at 0.5 (linear,
    partition-invariant — the ONLY fixture the sample-exact claim is made for;
    stateful plugins differ across 4096-chunk vs 1024-block partitions and are
    NOT claimed) over a position-encoded input, PLAYING with a synthetic engine
    position, no automation curve: output blocks equal the frozen render of the
    same material through the same insert **sample-exactly** over the contiguous
    run; a seek mid-run causes exactly ONE reposition (counter) and the output
    re-aligns; STOPPED: no sequenced material from a test feed reaches the
    processor (`feedEnabled=false`) while an injected live event does, and
    automation is held (a curve present but the value constant at
    `holdAutomationAt`); the sine fixture is used for presence only.
  - AC3 ring gate: a root page with `contentEpoch < flipEpoch` ⇒ the ring entry
    is NOT summed; on disarm a root page with `contentEpoch < flipEpoch'` ⇒ the
    ring IS still summed and stops once the re-summed page lands (unit test on
    the RT mixer function, extracted as a pure function like `twmonitor::*`);
    position mismatch ⇒ silence + counter.
  - AC4 `liveOwnedRefusals`: a `render(positional)` from a non-marked thread on a
    live-owned processor returns silence and counts; after `setLiveOwned(false)`
    it renders.
  - AC5 goldens; every playback/instrument/automation case green; `repeat_test`
    on `playback_test`'s cycle block N=50.
- **Orchestrator-reviewed:** everything in this phase.

### L1b — Live lane APP: audio monitoring through the chain  *(app shell/track/mixer/timeline, testkit)*
- **Entry:** L1a merged.
- **Modules:** `main/shell` (`SLivePlanBuilder`, arm/disarm edit paths, render-
  suspends-live, meters), `main/objects/mixer` (`isLiveOwned` wiring predicate),
  `main/objects/track` (`trackInput`, `monitorMode`, closure helpers, nested
  clip-mute), `main/timeline` (arm menu = input selector, monitor button, input
  meter), `main/servicesui` (Audio input combo real), `main/testkit`, docs, CLAUDE.md.
- **Deliverables:** design D3/D9 (audio half): closure computation; exclusion
  (topmost nulled at the mixer, nested `setClipMuted`; separate predicate);
  readahead re-rooted demands (one handle per frozen input root); plan rebuild
  triggers (§3); `trackInput`/`monitorMode`/`arm-track`/`set-track-input`/
  `set-monitor-mode` (absolute, undoable); `pumpMeters` while a live lane is ON;
  input meter + live-track meter publication; render suspends live lanes;
  testkit `assert-monitor-latency` (cross-correlation), `assert-audio-continuity`,
  `assert-render-policy`, `assert-input-meter`.
- **Gate (ACs)** (`RUN_SERIAL`, `SMARAGD_CAPTURE_SPEED=1`, `SMARAGD_AUDIO_INPUT_BACKEND=file:…`):
  - AC1 `monitor_through_chain.qxa`: track 0 armed, `trackInput=audio:file:0`
    (2 s 480 Hz sawtooth, known RMS), `tw.test.clap.gain` 0.5, transport STOPPED;
    `wait-ms 1500`; `dump-playback-capture` (device session) → RMS 0.5× (±3 %);
    bypass → 1.0×; a second unarmed track's clip is silent while stopped. Then
    (a) monitor **Auto**: press Play (not Record) → the clip appears and the
    INPUT STOPS (tape style; the live lane may go OFF and the exclusion is
    undone); (b) monitor **On**: press Play → the clip appears AND the input
    keeps sounding through the insert (the armed track's own clips silent).
  - AC2 `monitor_latency.qxa`: `assert-monitor-latency inputFile=… maxFrames=…`
    over the position-encoded input: measured lag ≤ input block + ring depth +
    3 output blocks (in frames, capture backend 1024) — number recorded in the
    test comment + STATE.md.
  - AC3 `monitor_folder_closure.qxa`: folder with an unarmed child (clip) and an
    armed child (`file:` input) through the folder's insert; play → capture holds
    both (sibling RMS band via the re-rooted demand; input via the insert);
    disarm mid-play → `assert-audio-continuity maxGapFrames=1024` on the sibling.
  - AC4 `arm_during_playback.qxa`: sawtooth on track 1 playing; arm/disarm track
    0 at 1 s / 2 s → track-1 material continuous (`assert-audio-continuity` with
    `maxStep` = 2× the source's own) and no double (RMS band unchanged in the
    flip windows).
  - AC5 `render_while_armed.qxa`: armed + monitoring; `render` → byte-identical
    to the unarmed render of the same project (live suspended); after the render
    monitoring comes back as a FRESH arm (plan rebuilt, never resumed) — the
    capture shows the input again.
  - AC6 `assert-render-policy liveThreadRefusals=0 liveOwnedRefusals=0` at the
    end of every new case; goldens; every existing case green.
  - AC7 `repeat_test.sh` on AC1, AC3, AC4 N=50 × workers {1,4,8,16}.
  - **Not gated:** real device latency/jitter; WASAPI shared under load; ASIO;
    hearing an armed track's own clips.
- **Orchestrator-reviewed:** exclusion wiring + drain call order; the plan builder's closure; the render-suspend path.

### L2 — Live instruments (MIDI in) = 37 P8a
- **Entry:** L1b merged.
- **Modules:** `tw303a/events` (`twLiveEventSource`), `tw303a/plugins` (protocol;
  CONTRACT), `tw303a/devices` (keyboard in-process port; MIDI input fan-out rings;
  scheduler immediate thru ring), `main/shell`, `main/objects/track` (feed member
  while live-owned), `main/eventui` (dock → port; `virtual-key hold/release`),
  `main/servicesui`, testkit, docs.
- **Deliverables:** design D4/D8 (thru)/D9 (MIDI half): per-consumer rings at
  the MIDI device callback; `twLiveEventSource` (live clock mapping − input
  latency, rebase, clamp late to 0, held-note table for the chase); arm =
  exclusion → `retireComponentNodes` → `forgetContinuity()` → `setLiveOwned` →
  `setLiveEventSource(live)` on the CONSUMING processor (a folder instrument
  fed by a MIDI-armed child: the folder's slot 0) — never a `setEventSource`
  swap, never a member of `eventFeed()`; disarm order per D4; thru through the immediate ring; the keyboard port; Options MIDI inputs
  active; `virtual-key hold`/`release`/`durationMs`.
- **Gate (ACs):**
  - AC1 `live_instrument_play.qxa`: instrument track (`tw.test.clap.sine`), armed,
    `trackInput=midi:capture:any`, STOPPED; `midi-in-event` NoteOn 60 → 261.6 Hz in
    the capture within ring depth + 3 output blocks (measured, recorded); NoteOff → silence.
  - AC2 `live_instrument_merge.qxa`: PLAYING; sequenced E4 (1–2 s) + injected C4 at
    1.2 s → both fundamentals in second 1 (two-tone RMS closed form); the sequenced
    note NOT restarted (energy continuity across 1.2 s); while STOPPED the same
    project sounds NOTHING sequenced (feed masked).
  - AC3 `live_instrument_disarm_playback.qxa`: play; arm at 1 s, inject, disarm at
    2 s; from 2.5 s on the capture matches a no-arm playback capture of the same
    project by per-block RMS bands and `assert-audio-frequency` (two real-time
    captures are not byte-comparable — device-start locator and underruns
    differ), plus `assert-audio-continuity` across the disarm point; and a
    subsequent render equals a fresh-process render byte for byte.
  - AC4 `live_instrument_ownership.qxa`: with the instrument armed, a concurrent
    NON-ROOT demand (a preview/asset capture of that track) → `liveOwnedRefusals`
    ≥ 1 and the audible path unaffected; after disarm the same demand renders audio.
  - AC5 thru: `midiOutPort=capture`; injected notes appear on the capture MIDI
    OUT ≤ 5 ms after injection (`assert-midi-out` host-time delta — the same
    bound `devices_midi_test` holds for the sender; the measured value is recorded).
  - AC6 `virtual-key hold` C4 audible via the keyboard port; `release` → silence;
    stopped-transport step input still works.
  - AC7 goldens; instrument cases; `repeat_test` on AC1/AC2/AC3; `assert-render-policy`.
- **Orchestrator-reviewed:** the ownership sequence; the live source mapping; the thru ring.

### L3a — Capture bridge ENGINE  *(tw/sources, tw/record, tw/devices)*
- **Entry:** L1a merged (runs in parallel with L1b).
- **Deliverables:** `twGrowingCaptureSource` (chunked planar, atomic frontier,
  reader API by position), the bridge (input ring → pages + WAV writer with
  backpressure counters), `RecordingSession` refactor to a bridge consumer;
  `record_bridge_test`.
- **Gate:** pages == WAV == input file sample-exact for a paced `file:` input;
  a WAV sink stalled artificially never stalls the ring (counter, file finalised
  from pages); wide (2/6 channels).

### L3b — Audio recording APP
- **Entry:** L1b, L3a merged.
- **Deliverables:** `SRecordingContent` + the recording cut (frontier, preview
  extension; NO `invalidateRenderPathRange` walk to the root while its track is
  live-owned — one at disarm), the placement conversion + `recordStart` (D6)
  named once incl. the retrospective mapping when recording starts from a
  stopped transport, non-modal
  recording, punch region, loop takes (`add-take startOffset=`), `locatorHeldElsewhere`
  retired, `startRecording` via `setPlaying`, offsets/latency in Options,
  `record-start`/`record-stop`, `assert-recorded-clip`.
- **Gate:** `record_offset_zero.qxa` (position-encoded `file:` input with
  reported latency 4800; the placed clip's positions read true ± 1 block after
  compensation; `recordingOffsetMs=+20` moves it 960 frames earlier);
  `record_loop_takes.qxa` (cycle 0–2 s, 4.5 s → take stack of 2 + partial per
  proposal 17; one undo); `record_punch.qxa`; existing `takes_recording_placement`;
  waveform-while-recording (preview probe non-empty mid-record); goldens; sweeps.

### L4 — MIDI recording = 37 P8b
- **Entry:** L2, L3b merged.
- **Deliverables:** `SMidiRecorder` (transport-bounded, ONE `place-midi-recording`
  per pass; `SPlayheadClock` extracted from `SMidiOutPump`), `add-midi-take`,
  modes new-take/overdub/replace, loop takes, input quantise inside the macro,
  all-notes-off/chase on stop, retrospective capture optional (`place-retro-midi`).
- **Gate:** `midi_record_placement.qxa` (`midi-in-replay` 4 notes from 1 s while
  recording from 0.5 s → notes at their ticks ± 4096 frames; one undo removes
  the clip); `midi_record_modes.qxa`; `midi_record_loop_takes.qxa`; quantise
  1/16 on-grid; roundtrip rows; ACTIONS.md.

### L5 — Transport polish
- **Entry:** L1b (L3b for count-in placement).
- **Deliverables:** metronome click source in the plan (a live lane exists iff
  armed ∪ monitor ∪ metronome; renders never have one), count-in, pre-roll,
  latency readout, live-path plugin latency badge.
- **Gate:** click at the beat grid ± 1 block in the capture; render byte-identical
  to no-metronome; count-in shifts the placed clip by exactly N bars.

### L6 — *(outline)* multi-device duplex + drift, loopback wizard, ASIO validation (35).

## 4. Failure & flake protocol
A flake at any worker count is a bug. Suspects, in order: the arm/disarm drain
(the `liveOwned` counter names a worker touching a live processor), the epoch
gate vs the readahead's supersession, an input ring writer outliving its reader
at shutdown, a live source added between blocks (contiguity broken), the
recorder committing off the main thread. Pre-existing: the dangling-`SLink`
teardown family (fixed for `clip_properties_actions`), the `SActionHistory` pin.

## 5. Context & continuity
STATE.md + §6 + git are the durable state.

## 6. Progress tracker
| Phase | Status | Closed on | PR / commit | Notes |
|---|---|---|---|---|
| L0 input device layer + seams | ☐ | | | |
| L1a live lane engine | ☐ | | | |
| L1b live lane app: audio monitoring | ☐ | | | |
| L2 live instruments (37 P8a) | ☐ | | | |
| L3a capture bridge engine | ☐ | | | |
| L3b audio recording app | ☐ | | | |
| L4 MIDI recording (37 P8b) | ☐ | | | |
| L5 transport polish | ☐ | | | |
| L6 multi-device / ASIO validation (outline) | ☐ | | | gated on 35 |
