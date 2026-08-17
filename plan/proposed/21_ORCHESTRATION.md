# Proposal 21 — Orchestration plan (execution companion)

> **Status: PLAN v1 (2026-08-17)** for `21_REALTIME_DATAFLOW_INTEGRATION.md` v3.
> One Opus 5 sub-agent per phase (L0–L5), each in its own worktree/branch, each
> closed only when every acceptance criterion in its GATE is green; the
> orchestrator reviews the diff (the items marked *orchestrator-reviewed* line by
> line), runs the gate verdict and merges via PR. Same ground rules and loop as
> `37_ORCHESTRATION.md` §0–§2, restated briefly here because this proposal
> touches the RT path.

## 0. Ground rules

1. **The proposal is the spec** (D1–D8, §10). No re-litigating "no block pull /
   no small pages / exclusion by wiring / RT = root + ring / one action per
   pass". A genuine contradiction in code → STOP, write it into §13, surface it.
2. **Gates are hard.** Never widen a latency budget or an RMS band without a
   physically-grounded justification in the test comment and STATE.md; never
   touch a golden; never mark a flaky test expected-fail; **any RT/pump-thread
   violation of the render policy is a failure, not a warning**.
3. **Repo laws**: `./build.sh` (re-configure), `check_layering.py`,
   `check_logging.py`, `ctest -j4` from the repo root with the count reconciled;
   `repeat_test.sh` N ≥ 50 × `SMARAGD_REVAL_WORKERS` {1,4,8,16} for anything
   touching the scheduler, the readahead, the pump, the barrier or a class-1
   processor; **no Qt off the main thread**; POD thread_locals only; RT/pump
   never render; race fixes order-independent; playback/latency cases run
   `RUN_SERIAL` and at `SMARAGD_CAPTURE_SPEED=1`.
4. One phase per branch (`feat/21-l<N>-<slug>`), one PR; STATE.md entry at
   close with numbers and what was NOT gated (real hardware latency and jitter
   never are); tick §6.
5. Iterate until green; escalate after three genuinely different diagnoses, or
   for a hardware/ears judgment (real device monitoring, ASIO).
6. A task names its module set; touching another escalates.

## 1. Agent policy
Opus 5 implements each phase end to end (engine, app, tests, CONTRACT deltas,
STATE.md). The orchestrator keeps the verdict and reviews line by line: the RT
callback and ring, the pump loop, the exclusion wiring + drain, the placement
conversion, the recorders' commit. Recon/test authoring may fan out (read-only
or disjoint new files); implement OR verify, never both on one artifact.

## 2. The phase loop
ORIENT (brief + proposal sections + CONTRACTs + STATE.md tail) → PLAN → BUILD →
GATE (every AC + standing gate; reconcile counts) → FIX (attempt counter) →
CLOSE (STATE.md, tracker, PR with gated/NOT gated) → stop.

## 3. Phase briefs

### 3.0 Standing gate
```
./build.sh
python tools/check_layering.py
python tools/check_logging.py
ctest --test-dir smaragd/build -j4 --output-on-failure   # reconcile ctest -N
```
plus **byte-identical `smaragd/tests/goldens/`** (nothing armed ⇒ the pump does
not exist; a golden that moves is a design violation, never a re-freeze) and
`repeat_test.sh` sweeps on every new playback case.

Dependency graph:
```
L0 ──► L1 ──┬──► L2 ──┐
            └──► L3 ──┴──► L4 ──► L5
```

---

### L0 — Input device layer + test backends + two engine seams  *(tw/devices, tw/schedule, tw/graph guard, testkit)*
- **Entry:** none.
- **Modules:** `tw303a/devices` (+CONTRACT), `tw303a/schedule` (`retireComponentNodes`),
  `tw303a/graph` (`RenderPolicy` markers behind the existing `freezePage` check),
  `main/shell` (`main.cpp` env defaults under `--test-case`), `main/testkit`,
  `main/servicesui`/`SSettings` (recording offset keys only), `docs/contracts/THREADING.md`.
- **Deliverables:**
  1. `AudioInput` gains an event-driven **capture thread + SPSC ring** per open
     device (WASAPI: its own capture event, like the render side; CoreAudio/ALSA
     written and guarded, unverified on Windows — say so); `read()` stays as a
     ring pop for existing callers; the packet-tail drop (F7) is gone.
  2. `createAudioInput()` honours `SMARAGD_AUDIO_INPUT_BACKEND = file:<wav> | null
     | default` (mirror of `SMARAGD_AUDIO_BACKEND`); `FileAudioInput` replays a
     WAV **paced on the shared steady clock** (`MidiOutScheduler::hostNowNs()`),
     configurable `inputLatencyFrames`, loops or ends per option; `NullAudioInput`.
  3. `SMARAGD_MIDI_INPUT_BACKEND = capture | default`; `CaptureMidiInput` gains a
     thread-safe `inject(bytes, n, hostTimeNs)` used by two new verbs
     `midi-in-event` (`kind`/`key`/`velocity`/`channel` or `bytes`, `atFrame`
     optional = now) and `midi-in-replay` (`filePath` .mid or text log, `startFrame`).
  4. `CaptureRevalidator::retireComponentNodes(std::span<const twComponent*>)`:
     drop queued/ready nodes of those components, wait for Running ones (mirror
     of `retireObject`); `schedule_test` proves it.
  5. `twRtThreadGuard` → per-thread `RenderPolicy {Any, Never}` with
     `markRtThread()` / `markLiveThread()`; the one `freezePage` check reads it;
     a `liveThreadRefusals` counter for gates.
  6. Settings keys `audio/recordingOffsetMs/<device>`, `midi/inputOffsetMs/<port>`
     (no UI yet); THREADING inventory rows.
- **Gate (ACs):**
  - AC1 `devices_input_test`: (a) `FileAudioInput` over a 2 s position-encoded
    WAV delivers every frame exactly once in order (compare against the file)
    and its delivery host times track the clock within ±2 ms; (b) a synthetic
    packet larger than the caller's buffer loses NO frames through the ring
    (regression for F7); (c) `midi-in-event` injections come out of the capture
    input in order with their stamps; (d) env selection.
  - AC2 `schedule_test`: nodes of a retired component never execute after
    `retireComponentNodes` returns; a Running one is waited for; other
    components' nodes are unaffected; order-independent under 100 randomized
    interleavings.
  - AC3 `graph` unit test: a `markLiveThread` thread calling `freezePage` gets
    silence + `liveThreadRefusals` = 1 + one log; the RT marker still behaves
    exactly as before (`twRtThreadGuard` tests green).
  - AC4 `ctest -j4` 100 %; goldens by construction (no render path).
- **Orchestrator-reviewed:** the ring + capture thread; the guard change.

### L1 — The live lane: audio monitoring through the chain  *(tw/playback, tw/mix, tw/plugins guard, app shell/track/timeline, testkit)*
- **Entry:** L0 merged.
- **Modules:** `tw303a/playback` (+CONTRACT: `LiveGraphPump`, live ring, RT
  crossfade, live-only output state, `livePlan` handoff, horizon demands from the
  readahead), `tw303a/plugins` (`setLiveOwned` guard on the processor; NO change
  to `render()` semantics), `tw303a/mix` (`SStdMixer`-side exclusion helper if
  needed; CONTRACT identity note), `main/objects/mixer` (`reconnectTracksToMixer`
  audibility rule gains `isLiveOwned`), `main/objects/track` (`trackInput`,
  `monitorMode`, closure helpers, `applyChildTrackAudibility` for nested lanes,
  `beginRun` skip via `SApplication`), `main/shell` (`SLivePlanBuilder`,
  arm/disarm edit paths, `set-track-input`/`set-monitor-mode`/`arm-track` verbs,
  input + live meters), `main/timeline` (arm-button input menu; monitor button;
  input meter lane), `main/testkit`, docs/contracts, CLAUDE.md.
- **Deliverables:** design §2 D1/D2/D4/D7 (audio half) and §3: live plan =
  immutable snapshot {ordered live-owned tracks with their processors, gain
  envelopes, channel maps; folder sums with the frozen-input list; scratch sized
  for the widest track × device block}; exclusion via the wiring rule (top-level:
  null plug; nested: `setClipMuted`), epoch bumps as today; readahead issues
  re-rooted demands for the plan's frozen inputs; the pump thread (Qt-free,
  MMCSS, allocation-free steady state, `markLiveThread`) pops input rings, runs
  `processor->render(positional=true)` per slot in order (first block after arm
  is the reposition), `applyGain`, `channelMap`, folder sums with frozen root
  pages by position (`getPageIfExists` try-lock; miss = silence + counter),
  pushes the live ring; RT pops the ring, crossfades 2–3 ms at flip, adds to
  the root page; live-only output state (device runs while stopped when
  something is armed && monitoring; priming does not gate the ring); degraded
  fallback (`Unsupported` slot ⇒ input → gain → ring, badge); `trackInput`
  (`none|audio:<dev>:<mask>|midi:…|keyboard` — MIDI kinds parsed but inert until
  L2), `monitorMode auto|on|off`, `arm-track`, `set-track-input`,
  `set-monitor-mode` (absolute, undoable); input meter (pre-FX peak pair from
  the bridge) + live-track meter published position-keyed by the pump; `beginRun`
  skips live-owned; disarm applies the barrier; the head's arm right-click menu
  becomes the input selector.
- **Gate (ACs)** (playback cases `RUN_SERIAL`, `SMARAGD_CAPTURE_SPEED=1`, audio
  input backend `file:`):
  - AC1 `monitor_through_chain.qxa`: track 0 armed with `trackInput=audio:file`
    (a 2 s 480 Hz sawtooth WAV, RMS known), `tw.test.clap.gain` at 0.5 in slot 0,
    monitor on, transport STOPPED; `wait-ms 1500`; `dump-playback-capture` shows
    the input at 0.5× RMS (±3 %) — heard THROUGH the insert; with the insert
    bypassed 1.0×; a second, unarmed track's clip is silent (transport stopped).
  - AC2 `monitor_latency.qxa`: position-encoded input WAV → capture →
    `assert-source-position` yields a constant offset = input period + ring hop
    + pump lookahead + output buffer, measured and asserted ≤ (input block +
    3 output blocks) with the capture backend's fixed 1024 blocks; the number is
    written into the test comment and STATE.md.
  - AC3 `monitor_folder_closure.qxa`: folder with an unarmed child (clip
    playing) and an armed child; play; the capture contains BOTH (the sibling
    was frozen via the re-rooted demand — assert its RMS band) and the armed
    input through the parent's insert; disarm mid-play → the capture continues
    without a gap > 1 block (`assert-audio-continuity` verb: no window of
    silence longer than N frames inside a region that must be continuous).
  - AC4 `arm_during_playback.qxa`: play a sawtooth clip on track 1; arm track 0
    at 1 s and disarm at 2 s while playing → the capture's track-1 material has
    no discontinuity (max sample-to-sample jump within 2× the source's own) and
    the arm/disarm flips leave no window of silence.
  - AC5 goldens byte-identical (nothing armed); every `instrument_*`,
    `automation_*`, `midi_out_*`, `meter_*` case green (unarmed paths untouched).
  - AC6 `liveThreadRefusals == 0` and `liveOwnedRefusals == 0` across the whole
    suite (assert at process exit under `--test-case`: a new `assert-render-policy`
    verb or a runner check).
  - AC7 `repeat_test.sh` on AC1, AC3, AC4 N=50 × workers {1,4,8,16}, 100 %.
  - AC8 CONTRACT/docs: playback (live-only state, ring, crossfade), mix identity,
    schedule (horizon demands), plugins guard, track attributes, THREADING rows,
    CLAUDE.md "Recording Audio" intro paragraph rewritten to name the two lanes.
  - **Not gated (PR body):** real device latency/jitter; WASAPI shared under
    load; ASIO; hearing an armed track's OWN clips (design §10.1).
- **Orchestrator-reviewed:** the RT callback delta, the pump loop, the exclusion
  wiring + drain, the plan builder's closure.

### L2 — Live instruments (MIDI in) = 37 P8a  *(tw/events live source, tw/plugins live mode, app)*
- **Entry:** L0, L1 merged.
- **Modules:** `tw303a/events` (`twLiveEventSource`), `tw303a/plugins`
  (ownership protocol as designed: `setLiveOwned`, event source swap discipline;
  CONTRACT), `tw303a/devices` (in-process "Computer keyboard" `MidiInput` port),
  `main/shell` (arm path for MIDI inputs, MIDI-thru tee to `MidiOutScheduler`),
  `main/objects/track` (`trackInput=midi:<port>:<ch>|keyboard`), `main/eventui`
  (`SVirtualKeyboardDock` → port; `virtual-key hold/release/durationMs`),
  `main/servicesui` (MIDI page inputs become active), testkit, docs.
- **Deliverables:** design D3 + D7 (MIDI half): the device thread → SPSC event
  ring (+ tee); `twLiveEventSource::collect` (anchor mapping minus input latency,
  rebase, clamp-late-to-0, held-note table for the one chase); arm on an
  instrument track: drain (`retireComponentNodes`) → `forgetContinuity()` →
  `setLiveOwned(true)` → `setEventSource(merge{feed, live})` before the first
  block; disarm: flush + all-notes-off → restore feed → `forgetContinuity()` →
  re-wire → `invalidateRenderPathRange(armPos, ∞)`; `beginRun` skips live-owned
  (already in L1); MIDI-thru; the keyboard port; Options MIDI inputs active.
- **Gate (ACs)** (`SMARAGD_MIDI_INPUT_BACKEND=capture`, injections via verbs):
  - AC1 `live_instrument_play.qxa`: instrument track (`tw.test.clap.sine`), armed,
    `trackInput=midi:capture:any`, transport stopped; `midi-in-event` NoteOn 60
    at t0; capture shows 261.6 Hz within (input→audible budget = ring hop + pump
    lookahead + output buffer, asserted ≤ 3 output blocks and recorded); NoteOff
    → silence.
  - AC2 `live_instrument_merge.qxa`: a sequenced note (E4, 1–2 s) plays while
    a live C4 is injected at 1.2 s → both frequencies present in second 1
    (`assert-audio-frequency` on band-filtered energy or two-tone RMS closed
    form); the sequenced note is NOT restarted by the injection (energy
    continuity).
  - AC3 `live_instrument_disarm_rerender.qxa`: arm, inject, disarm; then a
    render of the same project is `assert-file-identical` to a render made in a
    fresh process (the live pass left no state behind; barrier order proven).
  - AC4 `live_instrument_ownership.qxa`: with an instrument armed, force a
    freeze-path demand at its position (`assert-meter` probe or a render of a
    range) → `liveOwnedRefusals` increments by ≥ 1 and the audible path is
    unaffected; after disarm the same probe renders audio.
  - AC5 MIDI-thru: track with `midiOutPort=capture`; injected notes appear on
    the capture MIDI OUT within ≤ 2 ms of injection (`assert-midi-out` with the
    host-time mapping).
  - AC6 `virtual-key hold` C4 → audible via the keyboard port; `release` →
    silence; the dock's stopped-transport step input still works.
  - AC7 goldens; instrument cases; `repeat_test` on AC1/AC2/AC3.
- **Orchestrator-reviewed:** the ownership sequence (every step, order-independence argument), the live source's time mapping.

### L3 — Capture bridge + audio recording refactor  *(tw/record, tw/sources capture, app recording paths, testkit)*
- **Entry:** L1 merged.
- **Modules:** `tw303a/record` (+CONTRACT), `tw303a/sources` (planar capture ctor
  use), `main/objects/cut` (recording cut frontier / preview growth), `main/shell`
  (`startRecording` rewrite, non-modal, punch, loop takes), `main/servicesui`
  (recording offset + latency readout), `main/model` (the placement conversion
  named once), testkit, docs.
- **Deliverables:** design D4/D5/§6: one input pump per active input feeding
  ring + wide capture pages (frontier; arranger draws the growing cut) + WAV;
  `RecordingSession` becomes a ring consumer; `onRecordingCompleted` uses the ONE
  placement conversion (`recordStart + k − inLatProj − outLatProj + userOffsetProj`);
  `locatorHeldElsewhere()` retired; punch region (filter on stamped material);
  loop record → one take per pass via `add-take startOffset=`; count-in hook
  (used by L5); Options: per-device recording offset + latency readout;
  `record-start`/`record-stop`; `assert-recorded-clip`.
- **Gate (ACs):**
  - AC1 `record_offset_zero.qxa`: `file:` input replays a position-encoded WAV
    with a configured `inputLatencyFrames` = 4800; record 2 s from locator 1 s;
    the placed clip's `assert-source-position` reads its true positions with
    ≤ 1 block error after compensation; with `recordingOffsetMs=+20` the clip
    moves 960 frames earlier.
  - AC2 `record_loop_takes.qxa`: cycle 0–2 s, record 4.5 s → a take stack of 2
    complete takes + the partial third handled per proposal 17 rules; `<undo
    count="1"/>` removes the whole pass.
  - AC3 `record_punch.qxa`: punch region 1–2 s inside a 3 s record → the placed
    clip spans exactly the region (± 1 block).
  - AC4 the existing `takes_recording_placement`, `place-recording` cases green;
    waveform-while-recording: `assert-clip-window`/preview probe on the growing
    cut mid-record (a `wait-ms` then a preview assertion) shows non-empty peaks.
  - AC5 goldens; `repeat_test` on AC1/AC2.
- **Orchestrator-reviewed:** the placement conversion; the bridge's three sinks' backpressure (WAV falling behind must not stall the ring).

### L4 — MIDI recording = 37 P8b  *(app shell recorder, objects/midi verbs, testkit)*
- **Entry:** L2, L3 merged.
- **Modules:** `main/shell` (`SMidiRecorder`, `SPlayheadClock` extracted from
  `SMidiOutPump`), `main/objects/midi` (`place-midi-recording`, `add-midi-take`,
  `place-retro-midi` optional), `main/objects/cut` (`STakeStack` MIDI takes via
  `SClipWindow` — verify homogeneity), `main/servicesui`/`SOpt` (record mode,
  input quantise, retro buffer), testkit, docs/ACTIONS.md.
- **Deliverables:** design D6: transport-bounded recorder on the main thread
  reading the event ring tee; host time → frame → ticks once; ONE
  `place-midi-recording` per pass (macro with `add-midi-take` / `insert-midi-clip`
  + optional `quantize-notes`); modes new-take/overdub/replace; loop takes; all-
  notes-off/chase on stop; retrospective capture optional (rolling ring +
  `place-retro-midi`).
- **Gate (ACs):**
  - AC1 `midi_record_placement.qxa`: `midi-in-replay` of a 4-note file starting
    at 1 s while recording from 0.5 s → `assert-recorded-clip kind=midi` and
    `assert-midi-events` show 4 notes at their ticks ± 4096 frames; `<undo count="1"/>`
    removes the clip.
  - AC2 `midi_record_modes.qxa`: overdub merges into an existing clip (count
    N+4), replace overwrites the covered range, new-take stacks (takeCount 2).
  - AC3 `midi_record_loop_takes.qxa`: cycle 0–2 s, two passes → take stack of 2.
  - AC4 input quantise 1/16 → every recorded tick on the grid; the macro is one
    undo step.
  - AC5 goldens; roundtrip rows; ACTIONS.md.
- **Orchestrator-reviewed:** the recorder's commit + undo shape; the tick mapping (once).

### L5 — Transport polish  *(app; a tiny live-path click generator)*
- **Entry:** L1 merged (L3 for count-in placement).
- **Deliverables:** metronome click generated ON THE LIVE PATH only (a click
  source in the pump's plan; renders untouched — the golden proves it),
  count-in bars (transport starts N bars early, recording start offset
  accordingly), pre-roll, latency readout in the transport bar, live-path
  plugin latency budget warning (sum of `reportedLatency()` over the live-owned
  chain vs a threshold — a badge, no PDC).
- **Gate:** click audible in the capture at the beat grid (± 1 block), a render
  of the same project byte-identical to the no-metronome render; count-in shifts
  the placed clip's start by exactly N bars; readout matches `meterLatencyFrames()`.

### L6 — *(outline)* multi-device duplex + drift compensation, loopback calibration wizard, ASIO validation (35).

## 4. Failure & flake protocol
A flake at any worker count is a bug. Suspects, in order: the arm/disarm drain
(a node touching a live-owned processor — the guard counter names it), the
crossfade window vs the readahead's epoch supersession, an input ring writer
outliving its reader at shutdown, `setEventSource` called between blocks, the
recorder committing off the main thread. Pre-existing families: the dangling-
`SLink` teardown segfault (fixed for `clip_properties_actions`, watch siblings),
the `SActionHistory` pin.

## 5. Context & continuity
STATE.md + §6 + git are the durable state; on resume read §6, STATE.md tail,
`git log -15`, the open PR.

## 6. Progress tracker

| Phase | Status | Closed on | PR / commit | Notes |
|---|---|---|---|---|
| L0 input device layer + test backends + seams | ☐ | | | |
| L1 live lane: audio monitoring through the chain | ☐ | | | |
| L2 live instruments (37 P8a) | ☐ | | | |
| L3 capture bridge + audio recording | ☐ | | | |
| L4 MIDI recording (37 P8b) | ☐ | | | |
| L5 transport polish | ☐ | | | |
| L6 multi-device / ASIO validation (outline) | ☐ | | | gated on 35 |
