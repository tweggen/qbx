# Proposal 36 — Orchestration plan (execution companion)

> **Status: PLAN (2026-08-15).** How to execute `36_MIDI_INSTRUMENTS_AUTOMATION.md`
> phase by phase. Each phase is implemented by **one Opus 5 sub-agent** in its own
> worktree/branch, against a written brief (§3), and closes only when every
> acceptance criterion in its GATE is green. The orchestrator (whoever runs this
> plan — a Claude Code session on the top model, or the requester) reviews the
> diff, runs the gate verdict, and merges via PR. To use: *"Read
> plan/proposed/36_ORCHESTRATION.md and execute the next phase"* (or a named
> phase). Progress lives in STATE.md, the tracker (§6) and git — never in a
> session's memory.

## 0. Ground rules (bind every phase, every agent)

1. **The proposal is the spec.** The eight decisions D1–D8 in
   `36_MIDI_INSTRUMENTS_AUTOMATION.md` §2 are settled — do not re-litigate
   ticks-vs-frames, no-track-kind, reset+chase+pre-roll, the fader move, or
   play-time MIDI-out mid-implementation. A genuine contradiction found in code:
   STOP, write it into the proposal's §11, leave the tree buildable, surface it.
2. **Gates are hard.** Never weaken a gate to pass it: no widening RMS/frequency
   bands without a physically-grounded justification written into the test comment
   and STATE.md; never touch the byte-exact identity gates without an AC that
   licenses the re-freeze and a written explanation of why the bytes moved; never
   mark a flaky test expected-fail.
3. **Repo laws** (verify, don't assume): `python tools/check_layering.py` and
   `python tools/check_logging.py` clean before every commit; the qxa suite green,
   run **from `smaragd/tests/cases/`** (CWD-relative fixtures); `./build.sh`
   re-configures (the qxa glob is `CONFIGURE_DEPENDS` — a new `.qxa` is otherwise
   never registered; reconcile registered vs run vs skipped); race gates via
   `smaragd/tests/repeat_test.sh <bin> <case> N [workers]` swept over
   `SMARAGD_REVAL_WORKERS` {1,4,8,16}, N ≥ 50, for anything touching the scheduler,
   a class-1 processor, the run barrier, or the readahead; no Qt off the main
   thread (a MIDI device thread that emits a signal deadlocks teardown); engine
   stays plain C++; POD thread_locals only (MinGW); the RT callback never renders
   (`twRtThreadGuard` quiet); race fixes order-independent (remove the latch, never
   force an ordering); the worktree must have the clap/vst3 submodules fetched
   (`ensure_submodules()`) or the plugin cases silently disappear — **check the
   registered-case count against main**.
4. **One phase per branch, one PR per phase** (`feat/36-p<N>-<slug>`), few logical
   commits, imperative subjects. Update `plan/STATE.md` with a dated entry at every
   phase close: what landed, gate results with numbers, deviations, what was NOT
   gated (concurrency/latency properties routinely have no bespoke gate — say so).
   Tick §6.
5. **Iterate until green.** implement → build → gates → diagnose → fix → repeat.
   Escalate only when (a) a gate has resisted three genuinely different diagnoses,
   (b) the fix would violate rule 1 or 2, or (c) an ears/hardware judgment is
   needed (a real third-party instrument, a real MIDI device, jitter). When
   escalating, leave the tree buildable and write the open state into STATE.md.
6. **A task names one module set** (`docs/ARCHITECTURE.md` working agreement).
   Each brief lists the modules it may touch; touching another escalates. Public
   header / invariant changes are called out in the PR body as their own item.

## 1. Agent policy

- **Opus 5 implements each phase end to end** — model, engine, tests, docs, the
  CONTRACT.md deltas the brief names, the STATE.md entry. The briefs are written to
  be complete: file locations, the exact rules, the exact ACs.
- **The orchestrator keeps** the gate verdict, the diff review, and everything the
  brief marks *orchestrator-reviewed*: the run barrier and any scheduler seam, the
  processor continuity protocol, the fader move's golden argument, the ABI header,
  the persistence policy change. An agent may implement these; the orchestrator
  reads that diff line by line before the PR.
- **Recon and test authoring may fan out** to Explore/Opus sub-agents inside a
  phase (read-only, or writing disjoint new files). Agents implement OR verify,
  never both on the same artifact ("never let an agent fix a test to make its own
  code pass").
- **Parallel phases** run in separate worktrees; a phase that depends on another
  starts from that phase's merged `main`. Two agents editing the same existing
  file run serialized.

## 2. The phase loop

```
1. ORIENT   Read the brief (§3), the proposal sections it names, STATE.md tail,
            the CONTRACT.md files of the modules it may touch. Confirm entry.
2. PLAN     Work breakdown as tasks; name what fans out.
3. BUILD    Implement in the phase's worktree; commit logical units.
4. GATE     Run EVERY AC in the brief plus the standing gate (§3.0). Reconcile the
            case count. Delegate the sweep, keep the verdict.
5. FIX      Red → diagnose → fix → GOTO 4 (attempt counter per distinct failure).
6. CLOSE    STATE.md entry with numbers; tick §6; PR with "gated / NOT gated";
            stop. The orchestrator merges.
```

## 3. Phase briefs

### 3.0 Standing gate (assumed by every phase)
```
./build.sh                                     # re-configure is load-bearing
python tools/check_layering.py
python tools/check_logging.py
ctest --test-dir smaragd/build --output-on-failure   # reconcile registered/run/skipped vs main
```
plus **byte-identical renders of the golden corpus**. Until proposal 35's
`smaragd/tests/goldens/` lands on `main`, the corpus for 36 is: every existing
`render_*`, `grain_*`, `exact_*`, `warp_*` and `plugin_*` qxa case rendered on the
pre-phase `main` binary and `cmp`'d against the phase binary in the same worktree
(store the pre-phase WAVs in the scratchpad, never in the repo, unless 35's
committed-goldens rule is on `main` — then use it). A re-freeze needs an AC that
licenses it and a written justification.

---

### P0a — Persistence tolerance + `SClipWindow`  *(app model; no engine, no UI)*
- **Entry:** none.
- **Modules:** `main/model`, `main/persistence`, `main/objects/cut`, `main/testkit`,
  `docs/contracts/CLIP_MODEL.md`, the two CONTRACT.md files.
- **Deliverables:**
  1. Loader policy: an `<SLink objectId>` whose target never resolves drops **the
     link**, warns once with the element name, and leaves the containing track and
     the project intact (`sprojectloader.cpp` leftover sweep). `<SProject
     formatVersion='2'>` written; readers default to 1 and only WARN on a higher
     value (persistence CONTRACT: "a reader must never refuse on version").
  2. `app/model/sclipwindow.h`: `SClipWindow { srcStart() (Fraction, source
     domain), duration() (frames), loopLength(), stretch() (Fraction),
     setWindow(...), cloneWindowOver(SProject*), timelineToSourceExact(...) }`.
     `SCut` implements it; **`SCut`'s body is otherwise untouched**.
     `split-clip`, `resize-clip`, `duplicate-clip`, `set-clip-name`,
     `place-clip`'s cut creation and `STakeStack` (`takeCutAt` → `takeAt`,
     `applyWindowAll`, `wrapCutLinkIntoStack`, `collapseSingleTakeStack`) dispatch
     on `dynamic_cast<SClipWindow*>` instead of `SCut*`. Pitch/formant/slip/warp
     stay `SCut`-only.
  3. `SObject::contentKind()` virtual (`Audio | Event`, default Audio) — declared
     now so P1 and the UI have one hook.
- **Gate (ACs):**
  - AC1 new `qxa` `load_unknown_object_survives`: a hand-written `.qxp` containing
    a bogus `<SFutureThing>` referenced by a link on track 0 loads; `assert-track-
    count` unchanged; the other clip on that track renders (RMS band); the log
    contains exactly one warning naming `SFutureThing`.
  - AC2 `sample_missing_survives`, every `take_*`, `split_*`, `render_split_*`,
    `stress_*` case green and **byte-identical renders** to pre-phase.
  - AC3 `action_roundtrip_test` green with no fixture change (this phase adds no
    verb).
  - AC4 `grep -rn "dynamic_cast<SCut\*>" main/objects/cut/src/s{split,resize,
    duplicate,setclipname}*.cpp main/objects/cut/src/stakestack.cpp` returns only
    the pitch/formant/slip sites listed in the PR body.
  - AC5 an old-format `.qxp` (no `formatVersion`) loads and re-saves with
    `formatVersion='2'`; a `.qxp` with `formatVersion='99'` loads with a warning.
- **Orchestrator-reviewed:** the leftover-sweep change (persistence inv. 6).

### P0b — `tw/events` engine leaf  *(engine leaf; core-only dependency)*
- **Entry:** none. Parallel with P0a and P2.
- **Modules:** new `tw303a/events/` (+ its CONTRACT.md), `tw303a/core/include/tw/
  core/twdomains.h`, `tw303a/CMakeLists.txt`, `tools/check_layering.py`.
- **Deliverables:** `twEventKind`, `twEvent`, `twEventSeq` (immutable sorted;
  `slice(a,b)`, `stateAt(P)` returning held notes {key, channel, velocity, start},
  sustain, last CC per (channel, cc), bend, program), `twTempoMap` (constant:
  `usPerQuarter`, `ppq`, `num/den`; `ticksToFrames(TickPos, srate) → Fraction`,
  `framesToTicks`; API shaped for segments), `TickPos` domain, `twSmf` (type 0/1
  read/write; PPQ rescale; running status; meta → kinds; unknown meta preserved),
  `twAutomationCurve` (`valueAt`, `fillRamp`, shapes step/linear/exp with tension),
  `events_test`.
- **Gate (ACs):**
  - AC1 `events_test`: (a) SMF corpus of ≥ 6 committed fixtures (type 0, type 1
    multi-track, running status, sysex, tempo/timesig/keysig/lyric meta, a 30 k-
    event file) round-trips byte-identically after import→export at the same
    PPQ, and re-imports to the same event table after PPQ rescale 480 → 960; (b)
    `stateAt(P)` equals a brute-force scan on 1000 random positions of a random
    2000-event sequence; (c) `ticksToFrames` at 44.1 k / 48 k / 96 k is exact for
    120 BPM (960 ticks = one quarter = 24000 frames @ 48 k) and round-trips
    `framesToTicks` exactly for every multiple of one tick; (d) curve `valueAt`
    matches closed forms within 1e-12 and `fillRamp` over [P, P+n) equals n calls
    of `valueAt`.
  - AC2 `check_layering.py` shows `events` as a leaf depending only on `core`;
    nothing in the dataflow DAG links it yet.
  - AC3 goldens byte-identical (no engine path touched — by construction; run
    the standing gate anyway).
- **Orchestrator-reviewed:** the public headers (`tw/events/*.h`).

### P1 — Event clips in the model  *(app; engine `tw/mix` container)*
- **Entry:** P0a and P0b merged.
- **Modules:** new `main/objects/midi/` (+ CONTRACT.md), `main/objects/track`
  (sync only), `main/model` (`contentKind` use), `main/persistence` (registry),
  `main/actions`/`docs/ACTIONS.md`, `main/testkit`, `main/timeline` (thumbnail
  renderer registration only), `tw303a/mix` (`twEventClipSet`), `tools/
  check_layering.py`.
- **Deliverables:** `SMidiSequence`, `SMidiCut` (proposal §3.1 exactly — ticks in
  content, frames on every track-facing side, listens to `bpmTempoChanged`),
  serialization (`<events><e …/></events>` inline, sorted on write, unknown kinds
  verbatim), verbs `insert-midi-clip`, `import-midi-file`, `export-midi-file`,
  `add-note`, `remove-note`, `set-notes`, `add-event`, `remove-event`,
  `set-events`, `quantize-notes`, `set-midi-cut`, `set-tempo` (replaces the two
  direct BPM writes); generic verbs (`split/resize/duplicate/move/take-*`) work on
  `SMidiCut` via `SClipWindow`; `twEventClipSet` in `tw/mix` (proposal §4.2 —
  `insert/update/removeClip` keyed `SLink*`, `collect(startPos, len)` with chase
  set + window clamp + synthesised note-offs at the clip end); `STrack` routes
  `contentKind() == Event` children into it and NOT into the bus mixers; every
  event edit → `invalidateRenderPathRange(a, ∞)`; `SMidiSequenceRendererInline`
  thumbnail; `contentKind()` replaces the `container` heuristic in
  `SCutRendererInline`; `.mid` in the insert filter + `text/uri-list` drops;
  testkit `assert-midi-events`, `assert-midi-file`; `action_roundtrip_test`
  fixtures for every verb; `docs/ACTIONS.md` rows.
- **Gate (ACs):**
  - AC1 `qxa` `midi_clip_roundtrip`: `import-midi-file` (a committed multi-track
    SMF) → `save-project` → `load-project` → `export-midi-file` → `assert-midi-
    file` note count / first note tick equal to the source; the exported SMF `cmp`-
    equals a committed expected file.
  - AC2 `qxa` `midi_clip_edit_verbs`: `add-note`/`set-notes`/`remove-note`/
    `add-event`/`quantize-notes`/`set-midi-cut` each change `assert-midi-events`
    as specified and undo exactly (verify-undo pass); a `split-clip` at a tick
    inside a note yields two cuts whose `assert-midi-events` counts sum to N+1 and
    whose straddling note is cut (dur of the head + dur of the tail = original);
    `resize-clip` stretch 2/1 doubles every note's frame position (`assert-midi-
    events at=` in frames); loop and slip through `resize-clip` behave as for
    audio (assert on the cut's snapshot positions).
  - AC3 `qxa` `midi_clip_tempo_remap`: `set-tempo` 120 → 60 doubles the MIDI cut's
    duration and every note's frame position, leaves an audio clip's `startTime`
    and duration on the same track unchanged, and undoes.
  - AC4 `qxa` `midi_clip_render_silent`: a project with only a MIDI clip renders
    (no instrument yet) with `assert-audio-energy maxRms=0.0001` and no `twview`
    "null component" warning in the log (grep the log via `assert-file-contains`
    on the run log — add a `assert-log-absent` verb if none exists).
  - AC5 old-build tolerance: a `.qxp` with a MIDI clip opens in the **P0a**
    binary (pre-P1) with the clip dropped and the audio intact (`load_unknown_
    object_survives` gets a second fixture generated by this phase).
  - AC6 `action_roundtrip_test` green with a fixture row per new verb; `docs/
    ACTIONS.md` updated; layering shows `objects/midi` at the rank of
    `objects/cut` and **no edge from `objects/track` to `objects/midi`**.
  - AC7 goldens byte-identical.
- **Orchestrator-reviewed:** `STrack` sync routing; `twEventClipSet::collect`'s
  window clamp / note-off synthesis; the tick→frame conversion site (exactly one).

### P2 — Plugin ABI events + in-repo instruments  *(engine `tw/plugins`; tests)*
- **Entry:** none. Parallel with P0/P1. Requires the clap/vst3 submodules fetched.
- **Modules:** `tw303a/plugins` (ABI header, backends, processor, registry, probe,
  tests), `tw303a/plugins/CONTRACT.md`.
- **Deliverables:** `tw/plugins/twpluginevents.h` + the `process()` overload,
  `capabilities()`, `audioOutBusCount/audioOutBus`, `tailFrames()` (proposal §5.1;
  no format types in the header); CLAP / VST3 / AU translation per §5.2 (note
  ports + dialect negotiation + note ids; VST3 kEvent bus activation, `IEventList`
  in/out, `IMidiMapping` CC→param points, `INoteExpressionController`,
  `ProcessContext`; AU `aumu/aumi` enumeration, `MusicDeviceMIDIEvent` before
  render, `AudioUnitScheduleParameters`, output elements); the processor's
  **generator modes** (mapping rows §4.3), per-chunk event slicing/rebasing, ONE
  sorted list per chunk (UI ring + host events), **reset + chase + pre-roll K**
  on discontinuity, instrument bypass keeps events, `kCacheEntries` 4 for
  instruments, `twProcessContext`; `tw.test.clap.sine`, `tw.test.clap.arp`, VST3
  `TestSine` (split component/controller), `twNativeInstrument` (`format="tw"`,
  `tw.native.303`); scanner `kScannerVersion` 2 + descriptor fields + probe JSON;
  `plugins_test` extensions.
  The processor is exercised in this phase **directly from `plugins_test`** (a
  synthetic `twEventSeq` and a fake tap position), not from the app.
- **Gate (ACs):**
  - AC1 `plugins_test`: for each of {native 303, CLAP sine, VST3 TestSine, AU (macOS,
    stock `aumu` — qualitative only)}: a NoteOn(60, vel 100) at frame 1000 of a
    65536-frame page yields silence before 1000 (peak < 1e-6), a fundamental of
    261.6 ± 1 Hz after (autocorrelation over 4096 frames), and RMS within ±2 % of
    the fixture's closed form for the sine plugins; NoteOff at 30000 → silence
    after (peak < 1e-6 from 30000+1 for the sine fixtures).
  - AC2 `plugins_test`: a `ParamValue(gain, 0.5)` at chunk-relative offset 1234
    produces a level step at exactly frame 1234 of that chunk (CLAP and VST3;
    the VST3 fixture ignores `setParamNormalized`, so only `inputParameterChanges`
    with the right `sampleOffset` passes).
  - AC3 `plugins_test`: the VST3 fixture is silent when the host does not activate
    the kEvent bus (the teeth) — a deliberately-broken host path in the test
    proves the assertion can fail.
  - AC4 `plugins_test` continuity: rendering pages 0,1,2 in order equals (byte-
    exact) rendering page 2 alone after a discontinuity with pre-roll K = 65536
    for the native 303 (deterministic voice) — proves reset + chase + pre-roll
    reproduces steady state; a held note across the boundary is present in page 2
    both ways.
  - AC5 `plugins_test`: `tw.test.clap.arp` fed one held key emits NoteOn/Off pairs
    on its grid into `twEventOut`; the count over one page equals the closed form.
  - AC6 every existing `plugins_test`/`plugins_scan_test` case green; every
    `plugin_*` qxa case green and its render **byte-identical** (effects
    untouched by construction — the legacy `process()` path is the same code).
  - AC7 `plugincache.json` from a v1 scan is invalidated and rescanned once;
    the probe reports the new descriptor fields for the three test modules.
  - AC8 CONTRACT.md: inv. 5/6/16 amended, the new invariants (note ids, one list /
    one dialect, VST3 event bus activation, instrument bypass keeps events)
    added, the discontinuity debt paragraph rewritten.
- **Orchestrator-reviewed:** `twpluginevents.h`; the continuity protocol; the
  mapping-table rows.

### P3 — Instrument tracks + the fader move  *(engine ↔ app; the dangerous one)*
- **Entry:** P1 and P2 merged. Read proposal 35's state on `main` first (§9.1):
  if 35-B4 has landed, the head tap is a wide component; else per-bus taps.
- **Modules:** `main/objects/track` (+ CONTRACT), `main/objects/mixer`
  (`SPluginChain` slot-0 rule), `tw303a/plugins` (chain wiring, pass-through sum),
  `tw303a/mix` (`twGainStage`, trackmix gain retirement), `tw303a/schedule` +
  `tw303a/render` + `tw303a/playback` (run barrier hook), `main/shell`
  (barrier registration), `main/pluginui` (browser filter, strip row),
  `main/timeline` (head "I", derived colour/glyph), `main/testkit`, docs/contracts
  (FREEZE_PROTOCOL class-1 paragraph, THREADING), CONTRACT.md files touched.
- **Deliverables:**
  1. Instrument slot rules: `insert-plugin` of an `isInstrument` descriptor → slot
     0, refused if one exists (`expectReject`), `reorder-plugin` refused across
     slot 0; `STrack::instrumentSlot()`; the head tap in generator mode with the
     **pass-through sum** of its bus input (D3/§4.3); the event feed
     `twEventClipSet::collect` → processor per page; every event edit reaches the
     instrument tap's epoch (verify by rendering after an `add-note`).
  2. **Run barrier** `beginRun(pos)`: registered class-1 processors (every
     `SPluginSlot` via `STrack` → `SApplication`), called from
     `RenderSession::start` and `AudioEngine::seekTo`/readahead jump BEFORE the
     first demand; `invalidatePagesInRange(pos, ∞)` on their taps + `lastEnd_`
     cleared. Never from inside a render.
  3. **`twGainStage`** per bus between the last tap and the rewire: the fader
     (`sfadercurve.h` dB), mute with a 1–2 ms ramp (structural plug-nulling stays
     for solo rules), class ∞; `STrack::onTrackVolumeChanged` targets it;
     `twTrackMix::trackGainDb_` forced to 0 dB (kept one release, removed in P5).
  4. UI minimum: browser Kind filter + "Add Instrument"; strip instrument-first
     row + `describeSlot kind=`; head "I" button opening the param editor;
     derived instrument glyph/colour.
  5. Testkit: `assert-instrument-slot`; capture-backend seek case support if
     missing (`seek` verb → locator).
- **Gate (ACs):**
  - AC1 `qxa` `instrument_sine_render`: MIDI clip with C4 (0–1 s), E4 (1–2 s),
    G4 (2–3 s) at velocity 100 through `tw.test.clap.sine` → render →
    `assert-audio-frequency` per second (261.6 / 329.6 / 392.0 ± 1 Hz, channel 0)
    and `assert-audio-energy` per second within ±3 % of vel/√2 (16-bit
    quantisation); silence in second 4 (maxRms 0.0001). Same case for
    `twtestvst3` TestSine and for `tw.native.303` (RMS band, frequency ± 2 Hz).
  - AC2 `qxa` `instrument_render_determinism`: two `render`s of the AC1 project
    in one run, plus a third from a fresh process (the runner's second
    invocation) — all three WAVs `cmp` equal.
  - AC3 `qxa` `instrument_seek_continuity` (capture backend): a note held 0–4 s;
    seek to 2.0 s; play 1 s; `dump-playback-capture`; `assert-audio-frequency`
    on the first 4096 frames of the capture shows the note (chase) and
    `assert-audio-energy` on frames 0–512 ≥ 90 % of steady-state RMS (pre-roll —
    no attack restart audible for the envelope-less fixture).
  - AC4 `qxa` `instrument_mixed_track`: an audio clip (`test_sawtooth.wav`, 0–1 s)
    and a MIDI note (1–2 s) on one track with the sine instrument → both audible
    (RMS bands per second); with the instrument removed the audio clip is
    unchanged (`cmp` against a no-instrument render of the same project).
  - AC5 `qxa` `instrument_edit_reaches_render`: render; `add-note`; render →
    the second render differs where the note is and `cmp`-equals elsewhere
    (compare per-second RMS; the frames before the note byte-equal — use
    `assert-audio-energy` bands and a partial `cmp` verb if needed).
  - AC6 (explicit non-goal) arp → instrument IN-APP is not part of P3: an arp is
    an effect with events OUT and slot 0 is the instrument; routing plugin event
    output to the next slot is P9's MIDI-FX work. The arp fixture is gated in
    `plugins_test` only (P2 AC5). The PR body says so.
  - AC7 **Fader move**: every existing golden **byte-identical** at 0 dB (by
    construction: unity gain both places) AND for the two fixtures with a
    non-unity fader through `twtestclap` gain/stereoskew (`render_sawtooth_with_
    effects`, `plugin_bypass_and_param`, `meter_postfader`): if any 16-bit sample
    differs, the phase STOPS and reports the diff (which frames, by how many
    LSB) — a re-freeze is licensed ONLY for a documented floating-point-order
    difference of ≤ 1 LSB, written into the test comment and STATE.md.
    `meter_postfader`/`meter_levels` stay green (the meter tap is post-fader
    either way).
  - AC8 `repeat_test.sh` on `instrument_seek_continuity` and
    `instrument_render_determinism`: N=50 × workers {1,4,8,16}, 100 % pass.
    A single failure is a bug (rule 3), not a flake.
  - AC9 `assert-plugin-strip` shows `kind=instrument` on row 0; a second
    `insert-plugin` of an instrument `expectReject`s; `reorder-plugin` across slot
    0 `expectReject`s; undo of the first insert removes the row.
  - AC10 CONTRACT/docs: FREEZE_PROTOCOL class-1 paragraph; mix/CONTRACT gain
    stage; plugins/CONTRACT pass-through rule; track/CONTRACT instrument rules;
    THREADING inventory unchanged (barrier runs on existing threads).
  - **Not gated (say so in the PR):** render vs playback bit-identity (never
    was); real third-party instruments (demo only); stereo (sink mono until 35-B5;
    channel 0 asserted only).
- **Orchestrator-reviewed:** the run barrier (every line), the pass-through sum
  wiring, the fader move and its golden argument, the event-edit → tap-epoch path.
- **Escalation notes:** if AC7 shows a diff > 1 LSB the pre-FX gain was not what
  the code claimed — stop and report; if AC3's chase is missing after a seek,
  suspect the readahead's cached-page reuse (F3) *before* the barrier
  registration.

### P4 — Event editor (piano roll) + virtual keyboard  *(UI)*
- **Entry:** P1 merged (P3 for the audible AC).
- **Modules:** new `main/eventui/` (+ CONTRACT.md), `main/shell` (dock, menu,
  keyboard), `main/timeline` (time-axis link, `describeTrackHead`), `main/testkit`,
  `main/servicesui` (nothing yet), `tools/check_layering.py`.
- **Deliverables:** `SEventEditorDock` (selection follower, fifth dock, bottom),
  toolbar, `SEventTimeRuler`, `SEventEditorView` base + static-initializer kind
  registry, `SPianoRollView` (draw/select/erase/move/resize notes, velocity lane,
  CC lane stack, marquee, keyboard nudge), `SEventTimeAxis` linked to the arranger
  zoom/scroll (toggle), grid divisions from `STimeGridSpec`/`SSnapSpec` + the tempo
  map, `quantize-notes` UI, `SVirtualKeyboardDock` (two octaves, REAPER key map,
  velocity, octave ±; must not steal Space) inserting notes at the locator through
  `add-note`; testkit `assert-event-editor` (`clip`, `kind`, `contains`, `grabPng`),
  `virtual-key`, `describeTrackHead`; all edits as `set-notes`/`set-events` batch
  actions with revert-then-action on release (timeline inv. 3).
- **Gate (ACs):**
  - AC1 `qxa` `piano_roll_edits`: `virtual-key` C4 at the locator → `assert-midi-
    events count=1 key=60`; a scripted drag (a `drag-note` verb mirroring `drag-
    clip-edge`, or `set-notes` from the editor's own path) moves it; velocity lane
    edit changes velocity; each undoes (verify-undo).
  - AC2 `assert-event-editor kind=pianoroll contains="notes=1|grid=1/16|
    linked=1"` after opening on the selected clip; switching the selection to an
    audio clip yields `contains="empty=1"`; `grabPng` written for both.
  - AC3 `describeTrackHead` at Full/Compact/Tiny densities shows the instrument
    "I" and automation "A" buttons only where the density rules allow (assert
    strings), no clipping (widths asserted).
  - AC4 end-to-end (needs P3): `virtual-key` C4, `render`, `assert-audio-
    frequency` 261.6 ± 1 Hz.
  - AC5 `action_roundtrip_test` rows for `virtual-key`/`drag-note`; layering:
    `eventui` at the rank of `pluginui`.
- **Orchestrator-reviewed:** the selection-follower re-entrancy (inv. 8/9).

### P5 — Automation model + engine  *(app model + engine `tw/mix`, `tw/plugins`)*
- **Entry:** P0b and P3 merged.
- **Modules:** `main/model` (`SAutomationLane`, `ParamRef`), `main/objects/track`,
  `main/objects/mixer` (`SPluginSlot` lanes), `main/objects/cut` (+ `objects/midi`)
  clip envelopes, `main/actions`, `main/testkit`, `tw303a/mix` (gain stage curves,
  per-clip gain envelope in `twTrackMix`), `tw303a/plugins` (param curves →
  events, `automationEpoch_`), CONTRACT.md files, `docs/ACTIONS.md`.
- **Deliverables:** `SAutomationLane` owner-held inline `<automation>` on
  `STrack` / `SPluginSlot` / `SCut` / `SMidiCut` (proposal §3.3); `ParamRef`
  spaces `self:` (Volume, Muted), `param:<id>`, `cut:` (Gain, VelocityScale,
  Transpose); verbs `add/remove-automation-lane`, `set-automation-mode`,
  `add/move/remove-automation-point`, `set-automation-points` (batch, `mergeKey`
  owner+target), `set-track-volume` on a Read lane → points at the locator;
  curve snapshots swapped under the owner mutex; `twGainStage::setCurve` per-
  sample ramps (Volume in fader space; Mute step with ramp); processor
  `setParamCurves` → per-chunk `ParamValue` events (chase at chunk start,
  breakpoints, 64-frame dense ramp) with `automationEpoch_` in the stamp;
  per-clip gain envelope applied to `childPage` before `mixFrom`; every lane edit →
  `invalidateRenderPathRange` (exact for the gain stage, `[a, ∞)` for slots);
  `twTrackMix::trackGainDb_` removed; testkit `assert-automation-value`; Trim/Read
  semantics (static value × curve) — mode UI is P6.
- **Gate (ACs):**
  - AC1 `qxa` `automation_volume_ramp`: a 4 s sawtooth clip; lane `self:Volume`
    with points (0 s, −60 dB) → (4 s, 0 dB) linear in fader space; render; per-
    second RMS ratios follow the closed-form ramp within ±3 %; the render `cmp`s
    equal across two runs; `assert-automation-value time=2s` equals the midpoint.
  - AC2 `qxa` `automation_mute_step`: `self:Muted` points 1 s→on, 2 s→off: RMS
    second 2 < 0.0001, seconds 1 and 3 unchanged vs no-lane render, and no click
    (peak within 1 % of the no-lane peak over the ramp windows).
  - AC3 `qxa` `automation_plugin_param`: `tw.test.clap.gain` on a track; lane
    `param:0` step from 1.0 to 2.0 at frame 70000 (mid-page-2, mid-chunk):
    `assert-audio-energy` over [69000, 70000) = base and over [70000, 71000) =
    2 × base (±1 %); repeat with `twtestvst3` (sampleOffset teeth); render `cmp`
    across two runs.
  - AC4 `qxa` `automation_clip_gain`: `cut:Gain` envelope on a cut fades it out;
    `move-clip` by 1 s moves the fade with it (RMS bands per second); a
    `duplicate-clip` copies the envelope; a take stack's inactive take keeps its
    own.
  - AC5 `qxa` `automation_edit_invalidates`: render; `move-automation-point`;
    render → differs only inside the edited span (per-second RMS equal outside);
    for a `param:` lane the pages AFTER the span may differ (class-1) — assert only
    before.
  - AC6 goldens without lanes byte-identical (curve absent ⇒ scalar path);
    `meter_*` green.
  - AC7 `action_roundtrip_test` rows; `docs/ACTIONS.md`; CONTRACT deltas
    (mix, plugins inv. 15 stamp, track lane ownership).
  - AC8 `repeat_test.sh` on `automation_plugin_param` N=50 × workers {1,4,8,16}.
- **Orchestrator-reviewed:** the invalidation ranges; the epoch-in-stamp change;
  the `set-track-volume`-on-Read rule.

### P6 — Automation UI  *(UI)*
- **Entry:** P5 merged.
- **Modules:** `main/timeline` (new `sautomationlane.{h,cpp}` painting +
  gestures; `STrackRow` sub-lane kind; head "A" button; fader read-value display),
  `main/objects/cut` renderer overlay, `main/pluginui` (gesture punch-in from
  `ParamGestureBegin/End` out), `main/testkit`.
- **Deliverables:** sub-lane kind + target + per-track shown-lanes set (pruning
  walk done once for all UI-state sets), `drawAutomationLane`, picker menu,
  gestures (add/drag/tension/marquee/delete) with revert-then-action, head "A"
  mode cycling + menu, Touch/Latch/Write recorder (buffers on the UI thread,
  commits ONE `set-automation-points` per gesture; plugin gestures punch in),
  fader/param editor show the read value while a Read lane exists, clip envelope
  overlay + gestures; testkit `drag-automation-point`, `automation-write-tick`,
  `assert-lane-alignment` over automation lanes.
- **Gate (ACs):**
  - AC1 `qxa` `automation_lane_gestures`: `drag-automation-point` adds/moves/
    deletes; each undoes; `assert-lane-alignment` head/lane identity holds with
    two automation lanes + a take lane shown.
  - AC2 `qxa` `automation_write_pass` (capture backend): Touch mode, `automation-
    write-tick`s at 0.5 s / 1.0 s / 1.5 s during playback, stop → exactly ONE
    undo entry; `assert-automation-value` at those times; playback after the pass
    follows the curve (dump + per-second RMS).
  - AC3 `describeTrackHead` shows mode glyphs at three densities; PNG grabs of a
    lane and of the head.
  - AC4 `sstdmixerview.cpp` line count does not grow by more than 100 lines
    (painting/gestures live in the new file) — a `wc -l` assertion in the PR.
- **Orchestrator-reviewed:** the recorder's single-action commit; re-entrancy.

### P7 — MIDI output  *(engine `tw/devices`; app pump; options)*
- **Entry:** P1 merged (P3 not required).
- **Modules:** `tw303a/devices` (+ CONTRACT), `main/shell` (pump, panic, settings),
  `main/objects/track` (`midiOutPort/Channel`, `set-track-midi-output`),
  `main/servicesui` (Options → MIDI page), `main/testkit`, `docs/contracts/
  THREADING.md`, `docs/ACTIONS.md`.
- **Deliverables:** `MidiOutput`/`MidiInput` interfaces + `createMidiOutput()`
  by `SMARAGD_MIDI_BACKEND`; backends WinMM (`winmm`), CoreMIDI (virtual ports +
  timestamps), ALSA sequencer (virtual ports + queue), **capture** (default under
  `--test-case`; records host time → project frame + bytes), null;
  `MidiOutScheduler` std::thread + SPSC ring; `MidiOutPump` (main-thread QTimer
  like `pumpMeters`; lookahead window; channel remap; chase CC/PC/bend on
  start/seek, note-on chase per setting; sustain-off + all-notes-off on stop/seek/
  panic; renders emit nothing); latency alignment via `meterLatencyFrames()`'s
  mapping + user offset; `set-track-midi-output`; Options MIDI page (`build/load/
  apply` mirroring Audio; inputs listed but inactive until P8; "Create virtual
  port" gated per platform); testkit `assert-midi-out`, `dump-midi-capture`;
  THREADING inventory row.
- **Gate (ACs):**
  - AC1 `qxa` `midi_out_capture`: notes C4 at 0 s, E4 at 1 s, G4 at 2 s on a track
    with `set-track-midi-output port="capture" channel="2"`; play 3.5 s (capture
    audio backend paces it); `assert-midi-out kind=noteon key=60 channel=2
    at=0 tolerance=4096`, likewise E4/G4, `count kind=noteoff` = 3 by 3 s.
  - AC2 `qxa` `midi_out_chase_and_stop`: seek to 1.5 s inside a held C4 with a
    CC1 = 100 at 0.5 s; play 0.5 s; capture shows CC1=100 first (chase), then
    NoteOn 60 (note-on chase ON in the test settings); stop → all-notes-off
    (CC123) and sustain-off (CC64=0) on channel 2 present after the last note.
  - AC3 goldens byte-identical (MIDI-out touches no render path); `render` with
    a MIDI-out track produces no capture events.
  - AC4 `assert-midi-out` `expectReject`s when the backend is not `capture`.
  - AC5 Options MIDI page `describe()` over the capture/null backend lists
    exactly the ports the backend reports; settings persist.
  - **Not gated (PR body):** WinMM send jitter (±1 ms by design), CoreMIDI/ALSA
    driver timestamps against hardware, virtual-port creation on Windows (needs
    loopMIDI — documented).
- **Orchestrator-reviewed:** the scheduler thread's shutdown/join path (rule 1:
  no Qt on it), the pump's lookahead/seek reset.

### P8 — Live MIDI input + recording  *(outline; write the brief when 21-P1 lands)*
Event ring from `MidiInput` (device thread → SPSC → the processor's merge point in
the live lane), computer keyboard as an input device, MIDI arm (reuse "R" + channel
menu), record to a new `SMidiSequence` with take stacks/loop passes, input
quantise as a commit-time edit. Gates to be written against 21-P1's LiveGraphPump
contract; the capture MIDI backend doubles as the headless input source.

### P9 — Follow-ups *(own proposals; named so P3/P5 leave the seams open)*
Return tracks for multi-out (`Output.bus[M]` + tap `(processor, bus ≥ N)`), tempo
segments (proposal 37 behind `twTempoMap`), MIDI-FX event routing between slots
(arp → instrument in-app), score/tab/tracker editor kinds, generic `widget-gesture`
seam, PDC.

## 4. Failure & flake protocol
A flake at any worker count is a real bug — never rerun-until-green. Suspects, in
order, for this proposal: the readahead's cached-page reuse across runs (F3) vs the
barrier; the processor's `lastEnd_` after a barrier; a lane/event snapshot swapped
without the owner mutex; a curve edit that bumped the app object but not the
consuming component (invisible to the scheduler); the shared cursor / non-atomic
refcount / Qt-on-worker / thread_local-dtor family from earlier proposals. If a
gate implicates pre-existing code, record it separately in STATE.md; fix in its
own commit if small.

## 5. Context & continuity
Tasks per phase checklist; STATE.md + §6 + git are the durable state. On resume:
read §6, STATE.md tail, `git log --oneline -15`, the branch's open PR, then code.

## 6. Progress tracker (edit in place at each CLOSE)

| Phase | Status | Closed on | PR / commit | Notes |
|---|---|---|---|---|
| P0a persistence tolerance + `SClipWindow` | ☐ | | | |
| P0b `tw/events` leaf | ☐ | | | |
| P1 event clips in the model | ☐ | | | |
| P2 plugin ABI events + in-repo instruments | ☐ | | | |
| P3 instrument tracks + fader move | ☐ | | | |
| P4 event editor + virtual keyboard | ☐ | | | |
| P5 automation model + engine | ☐ | | | |
| P6 automation UI | ☐ | | | |
| P7 MIDI output | ☐ | | | |
| P8 live MIDI input (outline) | ☐ | | | gated on 21-P1 |
| P9 follow-ups (outline) | ☐ | | | own proposals |
