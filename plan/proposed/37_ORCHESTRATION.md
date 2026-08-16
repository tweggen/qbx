# Proposal 37 — Orchestration plan (execution companion)

> **Status: PLAN v2.2 (2026-08-15).** v2.2 adds the folder event feed and the
> per-track MIDI-out offset (design §3.2.1, D6) to P0b/P1/P3b/P7.
> How to execute `37_MIDI_INSTRUMENTS_AUTOMATION.md`
> phase by phase. v2 re-cuts the phases per the adversarial review (design §13):
> P0a gains the two testkit verbs everyone leans on; P2 is ABI/backends/fixtures
> only and depends on P0b's event header; P3 is three PRs (fader move, instrument
> slot + feed, render barrier) sequenced after proposal 36-B4; every "undo" AC is
> explicit; no AC reads a log file.
>
> Each phase is implemented by **one Opus 5 sub-agent** in its own worktree/branch,
> against a written brief (§3), and closes only when every acceptance criterion in
> its GATE is green. The orchestrator (whoever runs this plan — a Claude Code
> session on the top model, or the requester) reviews the diff, runs the gate
> verdict, and merges via PR. To use: *"Read plan/proposed/37_ORCHESTRATION.md and
> execute the next phase"* (or a named phase). Progress lives in STATE.md, the
> tracker (§6) and git — never in a session's memory.

## 0. Ground rules (bind every phase, every agent)

1. **The proposal is the spec.** The eight decisions D1–D8 in
   `37_MIDI_INSTRUMENTS_AUTOMATION.md` §2 and the six v2 decisions in §11 are
   settled — do not re-litigate ticks-vs-frames, no-track-kind, reset+chase+
   pre-roll, the render-only barrier, the fader move, or play-time MIDI-out
   mid-implementation. A genuine contradiction found in code: STOP, write it into
   the proposal's §11, leave the tree buildable, surface it.
2. **Gates are hard.** Never weaken a gate to pass it: no widening RMS/frequency
   bands without a physically-grounded justification written into the test comment
   and STATE.md; never touch the byte-exact identity gates without an AC that
   licenses the re-freeze and a written explanation of why the bytes moved; never
   mark a flaky test expected-fail; never assert undo through the runner's
   verify-undo pass (it compares track counts only) — use `<undo count=…/>` plus a
   state assertion.
3. **Repo laws** (verify, don't assume): `python tools/check_layering.py` and
   `python tools/check_logging.py` clean before every commit; the qxa suite green,
   run **from `smaragd/tests/cases/`** (CWD-relative fixtures); `./build.sh`
   re-configures (the qxa glob is `CONFIGURE_DEPENDS` — a new `.qxa` is otherwise
   never registered; reconcile registered vs run vs skipped); race gates via
   `smaragd/tests/repeat_test.sh <bin> <case> N [workers]` swept over
   `SMARAGD_REVAL_WORKERS` {1,4,8,16}, N ≥ 50, for anything touching the scheduler,
   a class-1 processor, the barrier, or the readahead (`0` = legacy pull, where
   instruments are silent by design — never in an instrument sweep); no Qt off the
   main thread (a MIDI device thread that emits a signal deadlocks teardown);
   engine stays plain C++; POD thread_locals only (MinGW); the RT callback never
   renders (`twRtThreadGuard` quiet); race fixes order-independent (remove the
   latch, never force an ordering); the worktree must have the clap/vst3
   submodules fetched (`ensure_submodules()`) or the plugin cases silently
   disappear — **check the registered-case count against main**.
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
7. **Proposal 36 (multichannel) landed on `main` with PR #39 at B2.** Before P3a/P3b, read
   `git log main -- plan/proposed/36_MULTICHANNEL_SIGNAL_FLOW.md` and its tracker;
   they start only after 36-B4 has merged (design §9.1). If the orchestrator
   decides to re-cut against the per-bus world instead, that decision is written
   into STATE.md and the P3 briefs are re-issued — never drifted into.

## 1. Agent policy

- **Opus 5 implements each phase end to end** — model, engine, tests, docs, the
  CONTRACT.md deltas the brief names, the STATE.md entry. The briefs are written to
  be complete: file locations, the exact rules, the exact ACs.
- **The orchestrator keeps** the gate verdict, the diff review, and everything the
  brief marks *orchestrator-reviewed*: the barrier walk and any scheduler seam, the
  processor continuity protocol, the fader move's argument, the ABI header, the
  persistence policy change. An agent may implement these; the orchestrator reads
  that diff line by line before the PR.
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
plus **byte-identical renders of the golden corpus**. Until proposal 36's
committed `smaragd/tests/goldens/` is on `main`, the corpus for 36 is: every
existing `render_*`, `grain_*`, `exact_*`, `warp_*`, `plugin_*` and `meter_*` qxa
case rendered on the pre-phase `main` binary and compared with `assert-file-
identical` (P0a) — or `cmp` — against the phase binary in the same worktree
(store the pre-phase WAVs in the scratchpad, never in the repo, unless 36's
committed-goldens rule is on `main` — then use it). A re-freeze needs an AC that
licenses it and a written justification.

Dependency graph (P3a needs 36-B4 + P2; P3b needs P1 + P2 + P3a):
```
P0a ──────────► P1 ──┬──► P4 (audible AC needs P3b)
P0b ──┬───────► P1   ├──► P7
      └──► P2 ──┬────┤
(36-B4) ────────┴► P3a ┴──► P3b ──► P3c ──► P5 ──► P6
```

---

### P0a — Persistence tolerance + `SClipWindow` + two testkit verbs  *(app model; no engine, no UI)*
- **Entry:** none.
- **Modules:** `main/model`, `main/persistence`, `main/objects/cut`, `main/testkit`,
  `docs/contracts/CLIP_MODEL.md`, `docs/ACTIONS.md`, the CONTRACT.md files touched.
- **Deliverables:**
  1. **Loader prune-and-retry** (design D8a): iterate the leftover sweep to a fixed
     point with a per-element-kind policy — an unresolvable `<SLink>` inside a
     *container* element (`STrack`, `SStdMixer`, `STakeStack`) drops the LINK with
     one warning naming the missing element/object; a *window* object (`SCut`, later
     `SMidiCut`) whose content link is dead is dropped itself (its own placement then
     falls under the first rule on the next pass); the root link must resolve or the
     load fails. `<SProject formatVersion='2'>` written; readers default to 1 and
     only WARN on a higher value.
  2. `app/model/sclipwindow.h`: `SClipWindow` in the window's OWN units with a
     frame-facing read API — `duration()`, `loopLength()`, `startOffset()` (frames),
     `stretchOrRate()` (Fraction), exact setters that take TIMELINE frames and
     convert once inside the implementation (`setWindowFromTimeline(...)`),
     `timelineToSourceExact(...)`, `cloneWindowOver(SProject*)`, and a per-
     `contentKind` **wrap factory** `SClipWindow::wrapContent(SProject*, SObject&)`
     (registered by static initializer; `SCut` registers for `Audio`). `SCut`
     implements it; **`SCut`'s body is otherwise untouched**. EVERY cast site
     dispatches on the interface: `ssplitclipaction.cpp` (incl. the `className()`
     string compare at ~:110), `sresizeclipaction.cpp`, `sduplicateclipaction.cpp`,
     `ssetclipnameaction.cpp`, `sunsplitclipaction.cpp`, `sselecttakeaction.cpp`,
     `sremovetakeaction.cpp`, `stakehelpers.cpp`, `stakestack.{h,cpp}` (`takeCutAt`
     → `takeAt`, `applyWindowAll`, `wrapCutLinkIntoStack`, `collapseSingleTakeStack`),
     `place-clip`'s cut creation. Pitch/formant/slip/warp stay `SCut`-only.
     `STakeStack` gains the homogeneity rule (`add-take` refuses a different
     `contentKind`; `STakeStack::contentKind()` = its takes').
  3. `SObject::contentKind()` virtual (`Audio | Event`, default Audio).
  4. Testkit verbs: `assert-file-identical` (`actual`, `expected` — the attribute
     names PR #37 landed with; absolute paths ALLOWED, unlike `render`'s output
     name, so cross-process compares work; `maxReportedDiffs`; optional
     `startFrame`/`frameCount` for WAVs — byte compare of the sample data) and `assert-log`
     (`contains`, `minCount`="1", `maxCount`="-1", `level`="" over the in-process
     `TwLog` ring since case start). Rows in `action_roundtrip_test`;
     `docs/ACTIONS.md`.
- **Gate (ACs):**
  - AC1 `qxa` `load_unknown_object_survives`: a hand-written `.qxp` containing a
    bogus `<SFutureThing>` referenced by a link on track 0 alongside a real clip
    loads; `assert-track-count` unchanged; the real clip renders (RMS band);
    `assert-log contains="SFutureThing" minCount="1"`.
  - AC2 `qxa` `load_missing_sample_placed_survives`: a `.qxp` whose PLACED clip's
    `SPlainWave` file is missing loads with the track intact and the other clip on
    it audible; `assert-log contains="<the missing path>" minCount="1"`.
    (`sample_missing_survives` stays as is.)
  - AC3 every `take_*`, `split_*`, `render_split_*`, `stress_*`, `plugin_*`,
    `render_*` case green and **byte-identical renders** to pre-phase
    (`assert-file-identical` against the scratchpad copies).
  - AC4 `action_roundtrip_test` green with fixture rows for the two new verbs.
  - AC5 `grep -rn "dynamic_cast<SCut\*>\|\"SCut\"" main/objects/cut/src/` returns
    only the pitch/formant/slip/warp sites and the class-registration literal
    listed in the PR body (the split action's `strcmp(…className(), "SCut")` must
    be gone).
  - AC6 an old-format `.qxp` (no `formatVersion`) loads and re-saves with
    `formatVersion='2'`; a `.qxp` with `formatVersion='99'` loads and `assert-log`
    finds the warning.
  - (The mixed-kind `add-take` refusal is gated in **P1** AC2, once an Event-kind
    object exists; P0a only implements the rule.)
  - `assert-log` implementation note: raise the `TwLog` ring capacity under
    `--test-case` and count entries since the previous action, so a long render
    cannot evict the line under test.
- **Orchestrator-reviewed:** the sweep policy (persistence inv. 6); the interface.

### P0b — `tw/events` engine leaf  *(engine leaf; core-only dependency)*
- **Entry:** none. Parallel with P0a.
- **Modules:** new `tw303a/events/` (+ its CONTRACT.md), `tw303a/core/include/tw/
  core/twdomains.h` (+ `twFrameRange` in core), `tw303a/CMakeLists.txt`,
  `tools/check_layering.py`.
- **Deliverables:** **the one** `twEventKind`/`twEvent` (`tw/events/twevent.h`,
  exactly the struct pinned in design §4.1: `int64 time`, metadata kinds, payload
  offset/size into the owner's arena); `twEventSeq` (immutable sorted + owned
  payload arena; `slice(a,b)`, `stateAt(P)` → held notes {key, channel, velocity,
  start}, sustain, last CC per (channel, cc), bend, program); `twTempoMap` (constant: `usPerQuarter`, `ppq`, `num/den`;
  `ticksToFrames(TickPos, srate) → Fraction`, `framesToTicks`; API shaped for
  segments; the single tempo authority — `bpm()` derived); `TickPos` exact-rational
  domain; `twSmf` (type 0/1 read/write; PPQ rescale; running status; meta → kinds;
  unknown meta preserved; notes paired on write); `twAutomationCurve` (`valueAt`,
  `fillRamp`, shapes step/linear/exp with tension); **`twEventClipSet` +
  `twEventSource`** (design §4.2: opaque `void*` key, the module's OWN
  `twEventClipResolved {seq, MapPosFn}` record — never `tw/graph`'s
  `twResolvedClip` — `collect` with chase set + window clamp + synthesised
  note-offs; returns `twFrameRange`); **`twEventMerge`** (a `twEventSource` over N
  sources: k-way merge by time, chase = union of `stateAt`, note ids namespaced
  per source index in the high bits — design §3.2.1); `events_test`.
- **Gate (ACs):**
  - AC1 `events_test`: (a) SMF corpus of ≥ 6 committed fixtures (type 0, type 1
    multi-track, running status, sysex, tempo/timesig/keysig/lyric meta, a 30 k-
    event file): import → export → import yields an **equal event table**
    (order, ticks, kinds, payloads); files AUTHORED by `twSmf` round-trip
    byte-identically; PPQ rescale 480 → 960 → 480 is lossless for tick-aligned
    events; (b) `stateAt(P)` equals a brute-force scan on 1000 random positions of
    a random 2000-event sequence; (c) `ticksToFrames` at 44.1 k / 48 k / 96 k is
    exact for 120 BPM (960 ticks = 24000 frames @ 48 k) and round-trips
    `framesToTicks` exactly for every multiple of one tick; denominators stay
    within `Fraction`'s red line (`ppq·10⁶/gcd`), asserted; (d) curve `valueAt`
    matches closed forms within 1e-12 and `fillRamp` over [P, P+n) equals n calls
    of `valueAt`; (e) `twEventClipSet::collect`: a note held across a clip end
    yields a synthesised NoteOff at the clip end; a note starting before the
    window appears only in the chase set; a looped clip (`loopLength < duration`)
    repeats events at the loop period; slip shifts positions without touching the
    sequence; (f) `twEventMerge` over two sources emitting the same key on the
    same channel yields two NoteOns with distinct note ids and two NoteOffs; its
    chase set is the union; a source removed mid-stream is dropped cleanly.
  - AC2 `check_layering.py` shows `events` as a leaf depending only on `core`;
    nothing in the dataflow DAG links it yet.
  - AC3 goldens byte-identical (no engine path touched — by construction; run the
    standing gate anyway).
- **Orchestrator-reviewed:** the public headers (`tw/events/*.h`).

### P1 — Event clips in the model  *(app; the engine container comes from P0b)*
- **Entry:** P0a and P0b merged.
- **Modules:** new `main/objects/midi/` (+ CONTRACT.md), `main/objects/track` (sync
  only), `main/model` (`SLink::timebase`, `contentKind` use), `main/persistence`
  (registry), `main/actions`/`docs/ACTIONS.md`, `main/testkit`, `main/timeline`
  (thumbnail renderer registration, `SCutRendererInline` heuristic, the ruler
  "Set BPM" write at `sstdmixerview.cpp:1554`, Clip Properties `SMidiCut` page),
  `main/shell` (tempo box write at `smainwindow.cpp:1929`), `tools/check_layering.py`.
- **Deliverables:** `SMidiSequence`, `SMidiCut` (design §3.1 exactly — tick-native
  window; frames on every track-facing side; listens to `bpmTempoChanged` and
  `sampleRateChanged`; `durationSec` migration overridden), `SLink::timebase`
  (default `beats` for Event content, `time` otherwise; serialized only when
  non-default; a `beats` link carries exact `startTicks` as authority and derives
  `startTime` — `move-clip`/`duplicate-clip`/`place-*` convert once and store
  ticks; `set-tempo` re-derives frames for every `beats` link incl. nested
  containers/assets), the take-stack homogeneity refusal (`add-take` of a
  mismatched `contentKind` `expectReject`s), serialization (`<events><e …/></events>` inline, sorted on write,
  unknown kinds verbatim incl. their attribute map), verbs `insert-midi-clip`,
  `import-midi-file`, `export-midi-file`, `add-note`, `remove-note`, `set-notes`,
  `add-event`, `remove-event`, `set-events`, `quantize-notes`, `set-midi-cut`,
  `set-link-timebase`, `set-tempo` (the ONLY tempo write — the two direct writes
  are replaced; rescales `beats` links; inverse restores both); generic verbs
  (`split/resize/duplicate/move/take-*`) work on `SMidiCut` via `SClipWindow`;
  `STrack` owns one `twEventClipSet`, routes `contentKind() == Event` children
  into it (same slots, `SLink*` key) and NOT into the bus mixers; **the track
  FEED** (`STrack::eventFeed()`, a `twEventMerge` over the own set + every child
  track that passes events up — design §3.2.1: no instrument slot and no MIDI-out
  unless `midiRouting` overrides; muted / solo-excluded children contribute
  nothing, resolved with `ssolorules.h`; rebuilt on child add/remove/reparent,
  mute/solo change and routing change), `midiRouting` serialized +
  `set-track-midi-routing`; every event edit → `invalidateRenderPathRange(a, ∞)`; `SMidiSequenceRendererInline` thumbnail;
  `contentKind()` replaces the `container` heuristic; `.mid` in the insert filter +
  `text/uri-list` drops; Clip Properties page for `SMidiCut`; testkit `assert-midi-
  events`, `assert-midi-file`; `action_roundtrip_test` fixtures for every verb;
  `docs/ACTIONS.md` rows.
- **Gate (ACs):**
  - AC1 `qxa` `midi_clip_roundtrip`: `import-midi-file` (a committed multi-track
    SMF authored by `twSmf`) → `save-project` → `load-project` → `export-midi-file`
    → `assert-file-identical` against the source SMF; `assert-midi-file` note count
    equal.
  - AC2 `qxa` `midi_clip_edit_verbs`: `add-note`/`set-notes`/`remove-note`/
    `add-event`/`quantize-notes`/`set-midi-cut` each change `assert-midi-events` as
    specified; each is followed by `<undo count="1"/>` + the prior assertion; a
    `split-clip` at a frame inside a note yields two cuts: `assert-midi-events` on
    the head shows the note with its ORIGINAL duration (non-destructive), the head's
    snapshot end synthesises the note-off (assert through `collect`-backed
    `assert-midi-events at=… kind=noteoff-synth`), the tail does not sound it;
    `resize-clip stretch="2/1"` (rate) doubles every note's frame position;
    loop and slip through `resize-clip` behave as for audio; `add-take` of an
    audio file onto a MIDI cut `expectReject`s.
  - AC3 `qxa` `midi_clip_tempo_remap`: track 0 audio clip at 2 s, track 1 MIDI clip
    (`timebase=beats`) at 2 s with a note at tick 960; `set-tempo` 120 → 60: the
    MIDI link's `startTime` doubles, its duration doubles, the note's frame position
    doubles, the audio clip's `startTime`/duration are unchanged; `set-link-
    timebase time` then `set-tempo` back → the MIDI link stays; `<undo count=…/>`
    restores each step (assert positions after each undo).
  - AC4 `qxa` `midi_clip_render_silent`: a project with only a MIDI clip renders
    (no instrument yet) with `assert-audio-energy maxRms=0.0001` and
    `assert-log contains="twView::getComponent() returned nullptr" maxCount="0"`
    (the exact `twview.cc:35` text — re-check it before writing the case).
  - AC4b `qxa` `midi_folder_feed`: a folder (`group-track`) with two child tracks
    each holding a MIDI clip (C4 / E4) and no instrument: `assert-midi-events
    scope="feed" trackPath="<parent>" count=2`; `set-track-mute` child 0 → count 1;
    `set-track-solo` child 1 → count 1; `set-track-midi-routing none` on child 0
    → count 1; each step `<undo count="1"/>` + re-assert.
  - AC5 (orchestrator-run, cross-binary) a `.qxp` saved by this phase with a MIDI
    clip opens in the P0a binary with the clip dropped and the audio intact.
  - AC6 `action_roundtrip_test` green with a fixture row per new verb; `docs/
    ACTIONS.md` updated; layering shows `objects/midi` at the rank of `objects/cut`
    with engine deps `{events, core}`, the engine edge `events` granted to
    `model` (`twTempoMap` on `SProject`), `objects/track` (`twEventClipSet`) and
    `timeline` (the ruler), and **no edge from `objects/track` to `objects/midi`**; `grep -rn "bpmTempo_ =\|setBPMTempo(" main/` hits only
    `set-tempo`'s apply/inverse and the loader.
  - AC7 goldens byte-identical.
- **Orchestrator-reviewed:** `STrack` sync routing; the single tick→frame
  conversion site inside `SMidiCut`; `set-tempo`'s link rescale + inverse.

### P2 — Plugin ABI events + fixtures + native 303  *(engine `tw/plugins` at the `twPlugin` level; NO processor/chain/tap change)*
- **Entry:** P0b merged (`tw/events/twevent.h`). Parallel with P1. Requires the
  clap/vst3 submodules fetched.
- **Modules:** `tw303a/plugins` (ABI header, backends, registry/scanner/probe,
  tests, the native instrument), `tw303a/plugins/CONTRACT.md`,
  `tools/check_layering.py` (`plugins → events`).
- **Deliverables:** `tw/plugins/twpluginevents.h` (uses `tw/events` types) + the
  `process()` overload, `capabilities()`, `audioOutBusCount/audioOutBus`,
  `tailFrames()` (design §5.1; no format types in the header); CLAP / VST3 / AU
  translation per §5.2 (note ports + dialect negotiation + note ids; VST3 kEvent
  bus activation, `IEventList` in/out, `IMidiMapping` CC→param points,
  `INoteExpressionController`, `ProcessContext`; AU `aumu/aumi` enumeration,
  `MusicDeviceMIDIEvent` before render, `AudioUnitScheduleParameters`, output
  elements); `tw.test.clap.sine`, `tw.test.clap.arp`, VST3 `TestSine` (split
  component/controller), `twNativeInstrument` (`format="tw"`, `tw.native.303`,
  registered like `twPassThrough`); **`tw.test.clap.gain` gains a `Clip Threshold` param (landed as id 2 — id 1 was
  already `Report Block Size`; default 0 = off; > 0 = hard clip at ±threshold
  AFTER the gain)**
  — the order-sensitive fixture P3a needs; scanner `kScannerVersion` 2 + descriptor fields
  + probe JSON; `plugins_test` extensions driving `twPlugin::process` directly on
  instances. **Explicitly out:** `twPluginSlotProcessor`, `twPluginInsert`,
  `twPluginChain` (36-B4 rewrites them; P3b owns the generator modes).
- **Gate (ACs):**
  - AC1 `plugins_test`: for each of {native 303, CLAP sine, VST3 TestSine, AU (macOS,
    stock `aumu` — qualitative only)}: a NoteOn(60, vel 100) at offset 1000 of a
    4096-frame block sequence yields silence before 1000 (peak < 1e-6), a
    fundamental of 261.6 ± 1 Hz after (autocorrelation over 4096 frames), and RMS
    within ±2 % of the fixture's closed form for the sine plugins; NoteOff at
    offset 30000 → silence after (peak < 1e-6 from 30001 for the sine fixtures).
  - AC2 `plugins_test`: a `ParamValue(gain, 0.5)` at block-relative offset 1234
    produces a level step at exactly frame 1234 of that block (CLAP and VST3;
    the VST3 fixture ignores `setParamNormalized`, so only `inputParameterChanges`
    with the right `sampleOffset` passes).
  - AC3 `plugins_test`: the VST3 fixture is silent when the host does not activate
    the kEvent bus — a deliberately-broken host path in the test proves the
    assertion can fail.
  - AC4 `plugins_test`: `tw.test.clap.arp` fed one held key emits NoteOn/Off pairs
    on its grid into `twEventOut`; the count over 65536 frames equals the closed
    form.
  - AC5 `plugins_test`: reset determinism of the SINE fixtures — `reset()`,
    NoteOn at offset 0, render 8192 frames, twice ⇒ byte-identical; the 303: same
    protocol ⇒ byte-identical (deterministic reset state is a fixture requirement,
    written into the test).
  - AC6 every existing `plugins_test`/`plugins_scan_test` case green; every
    `plugin_*` qxa case green and its render **byte-identical** (the legacy
    `process()` path is the same code).
  - AC7 `plugincache.json` from a v1 scan is invalidated and rescanned once; the
    probe reports the new descriptor fields for the three test modules and the
    native instrument appears in the registry with `isInstrument`.
  - AC8 CONTRACT.md: the new invariants (note ids, one list / one dialect, VST3
    event bus activation) added; the processor paragraphs untouched (P3b).
- **Orchestrator-reviewed:** `twpluginevents.h`; the VST3 host support list.

### P3a — Fader post-FX  *(engine `tw/mix`; app wiring; docs)*
- **Entry:** 36-B4 merged (one wide page per component; per-bus chains retired)
  and P2 merged (the `clipThreshold` fixture). Read 36's tracker first (rule 7).
- **Modules:** `tw303a/mix` (+ CONTRACT), `main/objects/track` (fader/mute wiring),
  `main/testkit`, `docs/contracts/THREADING.md` (none), CLAUDE.md "Level meters"
  paragraph, `tw303a/metering/CONTRACT.md` note.
- **Deliverables:** `twGainStage` between the last tap and the rewire: scalar
  `gainDb` (fader, `sfadercurve.h`), mute with a 1–2 ms ramp (structural plug-
  nulling stays for solo rules), class ∞; freeze path AND `calcOutputTo` (legacy
  pull) implemented as `page × gain`; `STrack::onTrackVolumeChanged` targets it;
  `twTrackMix::trackGainDb_` forced to 0 dB (kept until P5); the "legacy pull does
  not see a gain change" caveat rewritten in CLAUDE.md / testkit CONTRACT (the
  rewire's producer is now the gain stage).
- **Gate (ACs):**
  - AC1 every existing golden **byte-identical** (no golden combines a fader with a
    plugin — verified in the review; the same multiply on the same page).
  - AC2 new `qxa` `fader_post_fx` — two cases: (a) VALUE: `test_sawtooth.wav`
    (first second RMS 0.0667 unprocessed) with `set-track-volume −6.02` and
    `tw.test.clap.gain` at 2.0 → RMS of the first second within ±1 % of 0.0667
    (the product commutes); bypassed → within ±1 % of 0.0333; `assert-meter` at
    0.5 s reads the post-fader level. (b) ORDER: `set-track-volume −6.02` and
    `tw.test.clap.gain` gain 1.0, `Clip Threshold` (param id 2) 0.5, on a fixture whose peak is
    ≥ 0.9 in its loudest second: pre-FX gain would leave that second unclipped
    (RMS = 0.5 × unprocessed), post-FX gain clips first (RMS strictly below that,
    against a closed form computed from the fixture and written into the test
    comment). This case FAILS on the pre-move binary — verify that once and
    record it in STATE.md.
  - AC3 `meter_levels`, `meter_postfader`, `assert-meter`-driven cases green
    WITHOUT the "set gain before first probe" workaround (a second fixture that
    sets the gain AFTER a first probe and asserts the new level). If the legacy
    pull still gates on a stale epoch somewhere else, the agent REPORTS it in
    STATE.md/the PR body rather than fixing outside this phase's modules.
  - AC4 mute via `set-track-mute` still nulls the plug (solo cases green); a
    render with mute toggled at page boundaries is unchanged vs pre-phase.
- **Orchestrator-reviewed:** the golden argument (float order identical);
  `calcOutputTo`.

### P3b — Instrument slot + event feed  *(engine ↔ app)*
- **Entry:** P1, P2, P3a merged; 36-B4 merged.
- **Modules:** `main/objects/track` (+ CONTRACT: `SPluginChain`/`SPluginSlot` slot-0
  rules, `STrack::instrumentSlot()`, event feed wiring, project end + tail),
  `tw303a/plugins` (processor generator modes, pass-through sum, chain head rule,
  `twEventSource*`, reset+chase+pre-roll, `twProcessContext`; + CONTRACT), `main/
  pluginui` (browser Kind filter + "Add Instrument", strip instrument-first row,
  `describeSlot kind=`), `main/timeline` (head "I", derived glyph/colour),
  `main/testkit`, `docs/contracts/FREEZE_PROTOCOL.md` (class-1 paragraph),
  `docs/ACTIONS.md`.
- **Deliverables:** design §4.3 in full: generator mapping rows on the wide page,
  head tap keeps its audio input and SUMS it, per-page `collect` → per-chunk slice
  → `process(…, events, eventsOut, ctx)`, ONE sorted list per chunk (UI ring +
  host events), reset + chase + pre-roll K on discontinuity with **K = min(max(4096,
  tailFrames(), P − start(earliest held note)), 4 s)** (design D4),
  `twPluginSlotProcessor::forgetContinuity()` (clears `lastEnd_/haveLastEnd_`
  under `mutex_`; exposed through `SPluginSlot` for P3c), instrument bypass keeps
  events, `tailFrames()` → project end, instruments freeze-only (`calcOutputTo` =
  silence + one log); slot rules (`insert-plugin` of an instrument → slot 0, second
  refused, `reorder-plugin` across slot 0 refused); event edit → tap epoch (via
  the chain bump); UI minimum; testkit `assert-instrument-slot`.
- **Gate (ACs):**
  - AC1 `qxa` `instrument_sine_render`: MIDI clip with C4 (0–1 s), E4 (1–2 s),
    G4 (2–3 s) at velocity 100 through `tw.test.clap.sine` → render →
    `assert-audio-frequency` per second (261.6 / 329.6 / 392.0 ± 1 Hz, channel 0)
    and `assert-audio-energy` per second within ±3 % of vel/√2 (16-bit
    quantisation); silence in second 4 (maxRms 0.0001). Same for `twtestvst3`
    TestSine; for `tw.native.303`: frequency ± 2 Hz and RMS > 0.05 per note.
  - AC2 `qxa` `instrument_mixed_track`: an audio clip (`test_sawtooth.wav`, 0–1 s)
    and a MIDI note (1–2 s) on one track with the sine instrument → both audible
    (RMS bands per second); with the instrument PRESENT and NO notes the render is
    `assert-file-identical` to the no-instrument render (`x + 0.0f == x`).
  - AC3 `qxa` `instrument_edit_reaches_render`: a MIDI clip whose only note is at
    3 s (nothing sounds before 2 s — the open-ended invalidation re-renders page 1
    with reset+chase, which must be inaudible, so no held note may cross it);
    render A; `add-note` at 2 s; render B → `assert-file-identical` A vs B over
    frames [0, 96000) and RMS differs in second 3; `<undo count="1"/>`; render C
    `assert-file-identical` to A.
  - AC4 `qxa` `instrument_transpose_and_velocity`: `set-midi-cut transpose="12"`
    doubles the frequency; `velocityScale="0.5"` halves the RMS (sine fixture).
  - AC5 `qxa` `instrument_bypass_keeps_voices`: bypass at 0.5 s inside a 0–2 s
    note, un-bypass at 1.0 s: second 1.0–1.5 s has the note (energy band), no
    "resurrected" tail after 2 s.
  - AC6 `assert-plugin-strip` shows `kind=instrument` on row 0; a second
    `insert-plugin` of an instrument `expectReject`s; `reorder-plugin` across slot 0
    `expectReject`s; `<undo count="1"/>` of the first insert removes the row.
  - AC7 project end: a track whose last MIDI clip ends at 3 s with a `tailFrames()`
    = 24000 instrument reports `getDuration()` = 3.5 s (assert via `render` length).
  - AC7b `qxa` `instrument_folder_drums`: the sine instrument on a folder parent;
    two children (no instrument) with C4 in second 1 and E4 in second 2 → both
    frequencies in their seconds; `set-track-mute` child 0 → second 1 silent,
    second 2 unchanged; a child playing the same key at the same time as the
    other yields RMS ≈ 2× one voice (two overlapping notes, not one).
  - AC8 `repeat_test.sh` on `instrument_sine_render` and `instrument_mixed_track`:
    N=50 × workers {1,4,8,16}, 100 % pass (never `0`).
  - AC9 goldens byte-identical (no track without an instrument changes path);
    `plugin_*` cases green.
  - AC10 CONTRACT/docs: plugins inv. 5/6/16 amended, discontinuity paragraph
    rewritten, "instruments freeze-only" + `REVAL_WORKERS=0` note in testkit
    CONTRACT; FREEZE_PROTOCOL class-1 paragraph; track CONTRACT instrument rules.
  - **Not gated (PR body):** stereo (sink mono until 36-B5; channel 0 only); real
    third-party instruments (demo only); render-vs-playback identity; arp →
    instrument in-app (P9).
- **Orchestrator-reviewed:** the continuity protocol; the pass-through wiring;
  the event-edit → tap-epoch path.

### P3c — Render barrier + determinism  *(app `main/shell`; docs)*
- **Entry:** P3b merged.
- **Modules:** `main/shell` (`SApplication::beginRun`, call sites), `main/objects/
  track` (registration helper), `main/testkit`, `docs/contracts/FREEZE_PROTOCOL.md`,
  `tw303a/schedule/CONTRACT.md` (a note that the barrier is NOT a scheduler feature).
- **Deliverables:** `SApplication::beginRun(pos)`: main-thread walk over tracks
  whose slot 0 is an instrument → `invalidateRenderPathRange(pos, INT64_MAX)` +
  `slot->forgetContinuity()`; called from `startRender` before the session
  spawns, and from play start immediately before `t3Speaker_->startOutput()`
  (which performs the engine's pre-readahead `seekTo(locator)` + `startReadahead()`
  on the main thread — so the barrier precedes the first demand). NOT from
  `setGlobalLocatorPos` (a stopped locate demands nothing; `requestSeek` only runs
  while playing). Never from the readahead thread; never on a seek during
  playback or a loop wrap.
- **Gate (ACs):**
  - AC1 `qxa` `instrument_render_determinism`: render A; `toggle-playback` for
    ~2 s (capture backend) from 0.5 s; stop; render B; `assert-file-identical` A B;
    a second runner invocation renders C, `assert-file-identical` A C (the
    orchestrator wires the cross-process compare via the scratchpad).
  - AC2 `qxa` `instrument_locate_continuity` (capture backend): (a) sine: a note
    held 0–4 s; `set-locator` 2.0 s while stopped; play 1 s; `dump-playback-
    capture`; `assert-audio-frequency` on the first 4096 frames shows the note
    (chase); (b) 303 with `decay` long and a note held 0–4 s: the same locate at
    2.0 s; because K reaches back to the note's start (P − 0 = 2 s ≤ 4 s cap) the
    voice is pre-rolled from its own note-on, so RMS of frames [0, 2048) of the
    capture is within ±10 % of the RMS of frames [96000, 98048) of a full render.
    Without the held-note reach-back (K = one chunk) the chased note-on restarts
    the envelope and the band fails — verify that once by forcing K = 4096 in a
    debug knob and record the measured value in the test comment.
  - AC2c the AC1 render is `assert-file-identical` (absolute path) to a render
    made in a fresh process AFTER a play/stop cycle in that process — proves
    `forgetContinuity()`: a run starting exactly at the previous run's `lastEnd_`
    must not continue its voices.
  - AC3 `repeat_test.sh` on both, N=50 × workers {1,4,8,16}, 100 %.
  - AC4 goldens byte-identical (no instrument ⇒ no barrier effect).
  - **Not gated (PR body):** seek-during-playback splices; loop-wrap splices; the
    re-render cost (measure and record the number of pages re-rendered per play
    start on the AC1 project in STATE.md).
- **Orchestrator-reviewed:** every call site (thread, ordering before the demand).

### P4 — Event editor (piano roll) + virtual keyboard  *(UI)*
- **Entry:** P1 merged (P3b for the audible AC).
- **Modules:** new `main/eventui/` (+ CONTRACT.md), `main/shell` (dock, menu,
  keyboard), `main/timeline` (time-axis link, `describeTrackHead`), `main/testkit`,
  `tools/check_layering.py`.
- **Deliverables:** `SEventEditorDock` (selection follower, fifth dock, bottom),
  toolbar, `SEventTimeRuler`, `SEventEditorView` base + static-initializer kind
  registry, `SPianoRollView` (draw/select/erase/move/resize notes, velocity lane,
  CC lane stack, marquee, keyboard nudge), `SEventTimeAxis` linked to the arranger
  zoom/scroll (toggle), grid divisions from `STimeGridSpec`/`SSnapSpec` + the tempo
  map, `quantize-notes` UI, `SVirtualKeyboardDock` (two octaves, REAPER key map,
  velocity, octave ±; must not steal Space) inserting notes at the locator through
  `add-note`; testkit `assert-event-editor` (`clip`, `kind`, `contains`, `grabPng`),
  `virtual-key`, `drag-note` (the `drag-clip-edge` twin driving the REAL mouse
  handlers), `describeTrackHead`; all edits as `set-notes`/`set-events` batch
  actions with revert-then-action on release (timeline inv. 3).
- **Gate (ACs):**
  - AC1 `qxa` `piano_roll_edits`: `virtual-key` C4 at the locator → `assert-midi-
    events count=1 key=60`; `drag-note` moves it (assert new tick within the
    pixel-quantised range); a velocity-lane `drag-note` changes velocity; each
    followed by `<undo count="1"/>` + the prior assertion.
  - AC2 `assert-event-editor kind=pianoroll contains="notes=1|grid=1/16|linked=1"`
    after opening on the selected clip; switching the selection to an audio clip
    yields `contains="empty=1"`; `grabPng` written for both.
  - AC3 `describeTrackHead` at Full/Compact/Tiny densities shows the instrument
    "I" and automation "A" buttons only where the density rules allow (assert
    strings), no clipping (widths asserted).
  - AC4 end-to-end (needs P3b): `virtual-key` C4, `render`, `assert-audio-
    frequency` 261.6 ± 1 Hz.
  - AC5 `action_roundtrip_test` rows for `virtual-key`/`drag-note`; layering:
    `eventui` at the rank of `pluginui`.
- **Orchestrator-reviewed:** the selection-follower re-entrancy (inv. 8/9).

### P5 — Automation model + engine  *(app model + engine `tw/mix`, `tw/plugins`)*
- **Entry:** P0b and P3b merged.
- **Modules:** `main/model` (`SAutomationLane`, `ParamRef`), `main/objects/track`
  (track lanes AND `SPluginSlot` lanes — both live here), `main/objects/cut` (+
  `objects/midi`) clip envelopes, `main/actions`, `main/testkit`, `tw303a/mix`
  (gain-stage curves, per-clip gain envelope in `twTrackMix`, trackmix gain
  removed), `tw303a/plugins` (param curves → events, `automationEpoch_`),
  CONTRACT.md files, `docs/ACTIONS.md`.
- **Deliverables:** `SAutomationLane` owner-held inline `<automation>` on `STrack`
  / `SPluginSlot` / `SCut` / `SMidiCut` (design §3.3); `ParamRef` spaces `self:`
  (Volume, Muted), `param:<id>`, `cut:` (Gain, VelocityScale, Transpose); verbs
  `add/remove-automation-lane`, `set-automation-mode`, `add/move/remove-
  automation-point`, `set-automation-points` (batch, `mergeKey` owner+target),
  `set-track-volume`/`set-track-mute` on a Read lane → points at the locator;
  curve snapshots swapped under the owner mutex; `twGainStage::setCurves` per-
  sample ramps (Volume in fader space; Mute step with a 1–2 ms ramp); processor
  `setParamCurves` → per-chunk `ParamValue` events (chase at chunk start,
  breakpoints, 64-frame dense ramp) with `automationEpoch_` in the stamp;
  per-clip gain envelope applied to `childPage` before `mixFrom`; every lane edit →
  `invalidateRenderPathRange` (exact for the gain stage, `[a, ∞)` for slots);
  `twTrackMix::trackGainDb_` removed; testkit `assert-automation-value`; Trim/Read
  semantics (static value × curve) — mode UI is P6.
- **Gate (ACs):**
  - AC1 `qxa` `automation_volume_ramp`: a 4 s sawtooth clip; lane `self:Volume`
    with points (0 s, −60 dB) → (4 s, 0 dB) linear in fader space; render; per-
    second RMS ratios follow the closed-form ramp within ±3 %; two renders
    `assert-file-identical`; `assert-automation-value time=2s` equals the midpoint.
  - AC2 `qxa` `automation_mute_step`: `self:Muted` points 1 s→on, 2 s→off: RMS of
    [1.002 s, 1.998 s] < 0.0001; seconds 0 and 3 `assert-file-identical` to the
    no-lane render over those frame ranges; the RMS of the 2 ms window at each
    edge lies strictly between the two levels (a ramp exists, no hard step).
  - AC3 `qxa` `automation_plugin_param`: `tw.test.clap.gain` on a track; lane
    `param:0` step from 1.0 to 2.0 at frame 70000 (page 1, mid-chunk):
    `assert-audio-energy` over [69000, 70000) = base and over [70000, 71000) =
    2 × base (±1 %); repeat with `twtestvst3` (sampleOffset teeth); two renders
    `assert-file-identical`.
  - AC4 `qxa` `automation_clip_gain`: `cut:Gain` envelope on a cut fades it out;
    `move-clip` by 1 s moves the fade with it (RMS bands per second); a
    `duplicate-clip` copies the envelope; a take stack's inactive take keeps its
    own; `<undo count=…/>` after each step + assertion.
  - AC5 `qxa` `automation_edit_invalidates`: render A; `move-automation-point` on a
    `self:Volume` lane inside second 2; render B → `assert-file-identical` A B over
    seconds 0–1 and 3–4; differs in second 2; for a `param:` lane assert identity
    only BEFORE the edit (class-1, open-ended).
  - AC6 goldens without lanes byte-identical (curve absent ⇒ scalar path);
    `meter_*` green.
  - AC7 `action_roundtrip_test` rows; `docs/ACTIONS.md`; CONTRACT deltas (mix,
    plugins inv. 15 stamp, track lane ownership).
  - AC8 `repeat_test.sh` on `automation_plugin_param` N=50 × workers {1,4,8,16}.
- **Orchestrator-reviewed:** the invalidation ranges; the epoch-in-stamp change;
  the `set-track-volume`/`-mute`-on-Read rule.

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
    deletes; each followed by `<undo count="1"/>` + assertion; `assert-lane-
    alignment` head/lane identity holds with two automation lanes + a take lane.
  - AC2 `qxa` `automation_write_pass` (capture backend): Touch mode, `automation-
    write-tick`s at 0.5 s / 1.0 s / 1.5 s during playback, stop → `<undo
    count="1"/>` reverts ALL three values (`assert-automation-value` before/after);
    playback after the pass follows the curve (dump + per-second RMS).
  - AC3 `describeTrackHead` shows mode glyphs at three densities; PNG grabs of a
    lane and of the head.
  - AC4 `wc -l smaragd/main/timeline/src/sstdmixerview.cpp` grows by ≤ 100 lines
    vs the P5 merge base (painting/gestures live in the new file).
- **Orchestrator-reviewed:** the recorder's single-action commit; re-entrancy.

### P7 — MIDI output  *(engine `tw/devices`; app pump; options)*
> Executed as two PRs (2026-08-15): **P7a** = the `tw303a/devices` half (interfaces,
> backends, capture + audio-capture host-time log, `MidiOutScheduler`, unit test) —
> no P1 dependency, run in parallel with P1; **P7b** = the app half (pump, verbs,
> options page, qxa) after P1.
- **Entry:** P7a: none. P7b: P1 + P7a merged (P3 not required).
- **Modules:** `tw303a/devices` (+ CONTRACT; also the audio capture backend's
  host-time-per-block log), `main/shell` (pump, panic, settings), `main/objects/
  track` (`midiOutPort/Channel`, `set-track-midi-output`), `main/servicesui`
  (Options → MIDI page), `main/testkit`, `docs/contracts/THREADING.md`,
  `docs/ACTIONS.md`.
- **Deliverables:** `MidiOutput`/`MidiInput` interfaces + `createMidiOutput()` by
  `SMARAGD_MIDI_BACKEND`; backends WinMM, CoreMIDI (virtual ports + timestamps),
  ALSA sequencer (virtual ports + queue), **capture** (default under `--test-
  case`; records `{hostTimeNs, bytes}` only), null; the AUDIO capture backend
  records `{hostTimeNs, firstFrame}` per delivered block; `MidiOutScheduler`
  std::thread + SPSC ring, Qt-free join; `MidiOutPump` (main-thread QTimer, 20 ms,
  250 ms lookahead, de-dup cursor per track; chase CC/PC/bend on start/locate,
  note-on chase per setting, default OFF; loop wrap: split window, note-offs at
  the cycle end, chase at the cycle start; stop/locate/panic: sustain-off + all-
  notes-off per used channel; renders emit nothing); latency alignment via
  `meterLatencyFrames()`'s mapping + user offset; `set-track-midi-output`; Options
  MIDI page (`build/load/apply` mirroring Audio; inputs listed but inactive until
  P8; "Create virtual port" gated per platform; global MIDI-out offset default);
  per-track `midiOutOffsetMs` (`set-track-midi-output offsetMs=`, ±500, positive
  = earlier) applied by the pump; the pump reads the track FEED (children of a
  folder parent go out of the parent's port); testkit `assert-midi-out`
  (host time → project frame through the audio capture timeline), `dump-midi-
  capture`; THREADING inventory row.
- **Gate (ACs):**
  - AC1 `qxa` `midi_out_capture`: notes C4 at 0 s, E4 at 1 s, G4 at 2 s (0.5 s
    each) on a track with `set-track-midi-output port="capture" channel="2"`; play
    3.5 s at `SMARAGD_CAPTURE_SPEED=1`; `assert-midi-out kind=noteon key=60
    channel=2 at=0 tolerance=4096`, likewise E4/G4; `count kind=noteoff` = 3 by
    3 s; no event before the play start.
  - AC2 `qxa` `midi_out_chase_and_stop`: CC1 = 100 at 0.5 s; a C4 held 1–3 s;
    `set-locator` 1.5 s while stopped; play 0.5 s; capture shows CC1=100 first
    (chase), then NoteOn 60 (note-on chase ON in this case's settings); stop →
    CC123 and CC64=0 on channel 2 after the last note; with note-on chase OFF the
    NoteOn is absent.
  - AC3 `qxa` `midi_out_loop_wrap`: cycle 0–2 s, a note 1.5–2.5 s: capture shows
    NoteOff at 2.0 s (cycle end) then the next cycle's events from 0; no doubled
    NoteOn.
  - AC3b `qxa` `midi_out_offset_and_folder`: `offsetMs=200` on the AC1 track →
    every event lands 9600 ± 4096 frames EARLIER than in AC1; a folder parent
    with `port="capture"` and two children with notes → both children's notes on
    the parent's port, channel remapped to the parent's channel.
  - AC4 goldens byte-identical (MIDI-out touches no render path); `render` with a
    MIDI-out track produces no capture events.
  - AC5 `assert-midi-out` `expectReject`s when the backend is not `capture`.
  - AC6 Options MIDI page `describe()` over the capture/null backend lists exactly
    the ports the backend reports; settings persist.
  - **Not gated (PR body):** WinMM send jitter (±1 ms by design), CoreMIDI/ALSA
    driver timestamps against hardware, virtual-port creation on Windows (needs
    loopMIDI — documented), `SMARAGD_CAPTURE_SPEED ≠ 1`.
- **Orchestrator-reviewed:** the scheduler thread's shutdown/join path (rule 1),
  the pump's lookahead/locate/loop reset.

### P8 — Live MIDI input + recording  *(outline; write the brief when 21-P1 lands)*
Event ring from `MidiInput` (device thread → SPSC → the processor's merge point in
the live lane), computer keyboard as an input device, MIDI arm (reuse "R" + channel
menu), record to a new `SMidiSequence` with take stacks/loop passes, input
quantise as a commit-time edit. Gates to be written against 21-P1's LiveGraphPump
contract; the capture MIDI backend doubles as the headless input source.

### P9 — Follow-ups *(own proposals; named so P3/P5 leave the seams open)*
Return tracks for multi-out (`Output.bus[M]` + tap `(processor, out ≥ C)`), tempo
segments (proposal 37 behind `twTempoMap`), MIDI-FX event routing between slots
(arp → instrument in-app), `split-notes-at`, score/tab/tracker editor kinds,
generic `widget-gesture` seam, PDC, a playback-run barrier (needs an RT page-
boundary swap policy).

## 4. Failure & flake protocol
A flake at any worker count is a real bug — never rerun-until-green. **Known
pre-existing family, NOT ours (characterised 2026-08-15 by the render-duration
work, PR #34):** a worker-count-sensitive project-TEARDOWN segfault after PASS
in the dangling-`SLink` family — `warp_anchors_roundtrip`,
`exact_stretch_roundtrip` (~3/100 at `SMARAGD_REVAL_WORKERS=16`, 1/30 at 8,
0/30 at 4) and `lane_alignment` (a case with no `<render>` at all). If a 36 case
dies with that signature (crash strictly AFTER its assertions passed, at
teardown, worker-count dependent), record it against that issue in STATE.md and
do not spend the phase on it; a NEW crash inside a 36 code path is ours. Since
PR #34 a render is as long as the arrangement (no 60 s constant): a "silence
past the end" assertion must be aimed at the RENDER length, never at a frame
beyond it. Suspects, in
order, for this proposal: the readahead's cached-page reuse across runs (F3) vs the
render barrier's call site (main thread, BEFORE the demand); the processor's
`lastEnd_` after a barrier; a lane/event snapshot swapped without the owner mutex;
a curve/event edit that bumped the app object but not the consuming component
(invisible to the scheduler — F13); the shared cursor / non-atomic refcount /
Qt-on-worker / thread_local-dtor family from earlier proposals. If a gate
implicates pre-existing code, record it separately in STATE.md; fix in its own
commit if small.

## 5. Context & continuity
Tasks per phase checklist; STATE.md + §6 + git are the durable state. On resume:
read §6, STATE.md tail, `git log --oneline -15`, the branch's open PR, then code.

## 6. Progress tracker (edit in place at each CLOSE)

| Phase | Status | Closed on | PR / commit | Notes |
|---|---|---|---|---|
| P0a persistence tolerance + `SClipWindow` + verbs | ☑ | 2026-08-15 | `feat/36-p0a-clipwindow-loader` — `d6c8982`, `5866a7d`, `224f97a`, `a0517c5` | AC1/AC2/AC4/AC5/AC6 green; AC3 by targeted corpus compare. The full-suite reconciliation was NOT completed here (requester: parallelization + a `getDuration()` fix merge first) — see STATE.md 2026-08-15 |
| P0b `tw/events` leaf | ☑ | 2026-08-15 | 88c6758 (branch `feat/36-p0b-events-leaf`) | events_test 96 assertions / 0 failures; layering + logging clean; registered ctest 109 → 110. Full qxa reconciliation NOT completed — stopped by requester instruction pending suite parallelization; best partial run 43/110 green, the 5 failures in a second partial run were all 30 s render TIMEOUTS under six concurrent suites and had passed in the first. |
| P1 event clips in the model | ☑ | 2026-08-15 | `feat/36-p1-event-clips` — `e2fb1df`, `d7cdbae`, `402ef68`, `cd89d24` | AC1-AC4b, AC6, AC7 green. **ctest -j4: 124/124** (127 registered, 3 `au_*` disabled), goldens 69/69 byte-identical. AC5 is orchestrator-run — the fixture is `smaragd/build/midi_clip_roundtrip.qxp`. Two extra testkit verbs (`assert-clip-window`, the `noteoff*` kinds) and `events`+`objects/midi` also granted to testkit; `mode="channels"` and MIDI-out attributes deferred. See STATE.md 2026-08-15 |
| P2 plugin ABI events + fixtures + native 303 | ☑ | 2026-08-15 | `feat/36-p2-plugin-events` | AC1–AC8 green. `clipThreshold` is param id **2**, not 1 (id 1 was already the block-size reporter). AU implemented but UNVERIFIED (Windows). Full ctest NOT run — requester instruction. |
| P3a fader post-FX | ☑ | 2026-08-16 | `feat/36-p3a-fader-post-fx` | AC1–AC4 green. `twGainStage` (tw/mix) between the chain and the rewire; `twTrackMix::setTrackGain` a no-op until P5. **ctest -j4: 150/150 run passed, 153 registered** (151 before + 2 new cases), 3 `au_*` disabled, no flakes. AC1 verified by READING the golden fixtures (every `volume=` is '0') and by a 20-WAV pre/post `cmp` corpus over the fader/mute/plugin cases — 0 differing. AC2b **failed on the pre-move binary with exactly the predicted 0.274234**. AC3 note: the retired `assert-meter` caveat was ALREADY inert at the 36-B4 tip — the new case passes pre-move too; P3a makes it structurally impossible. New fixture `tests/test_clipsaw.wav` + its generator. See STATE.md 2026-08-16 |
| P3b instrument slot + event feed | ☐ | | | after 36-B4 |
| P3c render barrier + determinism | ☐ | | | |
| P4 event editor + virtual keyboard | ☑ | 2026-08-15 | `feat/36-p4-event-editor` | AC1/AC2/AC3/AC5 green; **AC4 (audible) SKIPPED - it needs P3b**, and belongs in a P3b case. ctest -j4: **128/128 run green** (131 registered, 3 `au_*` disabled); layering + logging clean. New module `main/eventui` at the rank of `pluginui` with NO edge to `timeline` (the axis link lives in the shell). Cases `piano_roll_edits`, `event_editor_dock`, `track_head_density`. CC lanes are draw-one-point (curve drawing is P6) and the `A` button is the seam only. See STATE.md 2026-08-15 |
| P5 automation model + engine | ☐ | | | |
| P6 automation UI | ☐ | | | |
| P7 MIDI output | ☑ | 2026-08-15 | `feat/36-p7a-midi-devices` + `feat/36-p7b-midi-out-app` — `3e29daa`, `536afeb`, `a6e4768` | **P7a (engine half) done 2026-08-15**: `tw/devices` MIDI interfaces + WinMM/CoreMIDI/ALSA-seq/capture/null backends + `MidiOutScheduler` + the audio capture backend's host-time block log; `devices_midi_test` green (max \|sent − due\| 1.32 ms over 40 runs). **P7b (app half) done 2026-08-15**: `SMidiOutPump` (20 ms main-thread QTimer, 250 ms lookahead, one `MidiOutScheduler` per port), `set-track-midi-output` + the three serialized `STrack` attributes, Options → MIDI page, five testkit verbs, seven qxa cases. AC1–AC6 green; measured lateness **−244 … +902 frames** against the 4096-frame budget. **ctest -j4: 131/132 run green, 135 registered** (128 + 7), 3 `au_*` disabled — the one failure is the pre-existing cold-plugin-cache teardown crash (`plugin_missing_placeholder`), characterised in STATE.md: across four full runs, 13 live plugin probes and 9 teardown crashes, every one inside a probing process and none among ~520 warm-cache runs. Two timing bugs found BY MEASUREMENT and fixed: anchoring on a position CHANGE rather than a PUBLICATION (first note 59 ms early) and opening the port on the first anchored tick (first note 153 ms late under load). CoreMIDI / ALSA-seq / WinMM-against-hardware remain UNVERIFIED (Windows, capture backend). |
| P8 live MIDI input (outline) | ☐ | | | gated on 21-P1 |
| P9 follow-ups (outline) | ☐ | | | own proposals |
