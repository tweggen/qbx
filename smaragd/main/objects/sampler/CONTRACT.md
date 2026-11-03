# app/objects/sampler — CONTRACT

Purpose: NassauDigital's MODEL in the app (proposal 56 §4.4, §4.5, milestone
Q1; nassau-digital `docs/DESIGN.md` §2, §2.1, §9.2, §9.3, §12).
`SSamplerProgram` (one instance's program, an `SObject` child of its plugin
slot), the resolver hook a verb finds it through (`ssamplerslots.h`), and the
program verbs: `add-zone`, `remove-zone`, `add-layer`, `remove-layer`,
`set-zone`, `set-layer`, `set-sampler-program`, `select-zones`, and the three
inverse forms `reinsert-zone`, `reinsert-layer`, `restore-sampler-values`.

Since Q2 (QBX-263) it also owns each layer's GENERATIONS (proposal 56 §4.6,
DESIGN §6.1): the jobs that decode a `file:` layer, their install, and the
bounded settle the offline render's barrier makes (invariants 12–16).

Since Q3 (QBX-266) a layer may LINK a qbx asset (proposal 56 §4.7, DESIGN
§6.2): the program holds the link (an `SLink` to the asset body), bounces the
asset into a generation when the asset's own content epoch moves, and refuses
a link that would be a render cycle (invariants 17–21). Since QBX-267 the
same walk refuses a LATER edit that would close such a cycle (invariant 22).

Since Q4 (QBX-268) a layer of a tempo-mode zone whose method is `stretch`
plays its source STRETCHED to the project tempo by qbx's phase vocoder, as a
generation, re-rendered on a tempo change and once more when the source's onset
analysis completes (proposal 56 §4.8, DESIGN §5, §13; invariants 23–27).

Since Q6 (QBX-272) a recording lands IN the instrument: `place-sampler-recording`
(DESIGN §7's landing rule), the instance state the rule reads — the VIEW, the
SELECTED PAD and its bank (`set-sampler-view`), the instance's own RECORD
INPUT (`set-sampler-record-input`), and the LAST KEY PLAYED, which the shell's
key tap writes (proposal 56 §4.10, §11 "Q6 as built"; the invariants headed
"Q6" below).

Since Q5 (QBX-271) an armed sampler track is PLAYED LIVE through the live
lane (proposal 56 §4.9, DESIGN §9.4). No line of this slice, of tw/sampler or
of the live lane changed for it: the sampler is a generator in slot 0 and
takes the path every instrument takes (invariant 28).

Public headers: app/objects/sampler/*.h

Depends on (engine): tw/core, tw/graph, **tw/sampler** (the editable program
`twSamplerProgramModel` and the snapshot it hands the instrument), **tw/plugins**
(the `twPlugin &` the instance initializer takes, reached as a `twSamplerHost`
by capability), **tw/sources** (since Q2: `twSampleSource`, the decoder every
sample in the app goes through, on a generation job's worker), **tw/pages**
(since Q3: the asset bounce freezes the content component's pages,
`twOutputPage`), **tw/sidecar** (since Q4: the onset analysis,
`twDetectOnsets`, and the `onsets` aspect it stores and looks up by the
source's content hash). The vocoder is reached through tw/sampler
(`twSamplerStretch`), not named here. App edges: {actions, model, persistence} — **the RANK of
`objects/midi`** (DESIGN §9.2; proposal 56 Q-6, answered by the author
2026-10-27). It does NOT depend on `objects/track`, although the slot it
configures lives there, and `objects/track` never depends on it (proposal 56
invariant 12). The SHELL joins the two (`main/shell/src/ssamplerwiring.cpp`).

Design: `plan/proposed/56_NASSAU_DIGITAL_SAMPLER.md` §4.4, §4.5, §11 (Q1).

## Invariants

1. **ONE MODEL, AND IT IS THE CORE'S** (A3, proposal 56 invariant 8). The
   truth is a `twSamplerProgramModel` — tw/sampler's pimpl over `nd::Program`.
   qbx never reads or writes a program attribute itself: every parameter, its
   range, the ALL scope, the clamp, the refusals and the XML are the core's.
   `SSamplerProgram` adds undo (through the verbs), persistence, signals and
   the hand-over to the instrument, nothing more.

2. **EVERY CHANGE GOES THROUGH `SSamplerProgram::edit()`**, which edits a COPY
   and commits it only when the edit succeeded: a refusal leaves the program
   bit-identical, however many core calls the edit made before refusing. A
   commit publishes ONE snapshot and emits `programChanged()`.

3. **AN EDIT REACHES EVERY INSTANCE, AND A REBUILT ONE IS BORN CURRENT**
   (proposal 56 invariants 3, 4). Each commit makes a new immutable
   `twSamplerProgram` snapshot and hands an initializer that installs it to
   the bound installer. The shell's installer sets it as the slot's instance
   initializer (future instances: a `setChannelCount()`, a `reloadPlugin()`, a
   rescan) and THEN applies it to the instances that exist through
   `SPluginSlot::applyToInstances()` (under the processor's mutex, then the
   path above the slot is staled). Never through the bare `plugin()` pointer.
   `publishedSerial()` is what every instance's
   `twSamplerHostStats::installedSerial` must equal.

4. **THE PROGRAM OBJECT EXISTS ONLY ONCE AN EDIT SUCCEEDED.** A NassauDigital
   slot has no `SSamplerProgram` until the first verb that changes it (the
   resolver creates it, `create == true`), and a refused `add-zone` creates
   none — the verb validates on a scratch copy first. A slot without one plays
   nothing, exactly as a slot with an empty program does, and saves nothing
   extra: every Q0 file and golden is unchanged. (Proposal 56 §4.4 said
   "created when a NassauDigital slot is inserted"; see §11 Q1.)

5. **UNDO IS ONE STEP PER VERB AND EXACT** (GATES Q1.1, C1.5). Every inverse
   restores ABSOLUTE values and is itself a registered verb, so it
   round-trips through XML:

   | forward | inverse | redo |
   |---|---|---|
   | `add-zone` / `add-layer` | `remove-zone` / `remove-layer` | RE-INSERTS the object the first apply made, same ids — a fresh add is never re-run |
   | `remove-zone` / `remove-layer` | `reinsert-zone` / `reinsert-layer` (the core's XML, at its index) | removes again |
   | `set-zone` / `set-layer` / `set-sampler-program` | `restore-sampler-values` (every replaced value of every target) | re-applies the edit, relative ones from the restored values |
   | `select-zones` | `select-zones mode="replace"` (the previous selection) | re-applies |

   qbx's history re-applies the FORWARD action on redo and re-uses one
   inverse instance for every undo (`SActionUndoCommand`), which is why the
   add verbs remember what they made: a redo that added afresh would mint new
   ids, and the redo of a later `set-zone zone="17"` would then refuse.

6. **IDS ARE NEVER REUSED** (DESIGN §2, Q1.q2). An undone `add-zone` leaves
   `nextId` where the add put it, so a new `add-zone` gets a new id. The undo
   is therefore exact in every zone, layer and parameter, and `nextId` is the
   one field that does not go back — which is what the gates assert
   (`equalsCaptured … ignoreNextId="1"` together with an explicit `nextId=`).
   `add-zone` MAY name an explicit zone `id` (QBX-265): accepted only when
   it is free and `>= nextId`, and `nextId` then moves past the zone and its
   layers, so an id is still never handed out twice; refused when in use or
   below `nextId` (the check is tw/sampler's `addZone()`). Its redo re-inserts
   the same ids like any add.

7. **EVERY REFUSAL IS ANNOUNCED AND MUTATES NOTHING**: `qWarning`,
   `<verb>: track T slot S: REFUSED: <why>`, the core's own reason where it is
   the core that refused.

8. **THE ROOT LIVES IN `SAction::pathRoot_`** (actions/CONTRACT.md inv. 6).
   A verb's `trackPath` may be root-qualified ("Drums:0"); a bare one resolves
   against `pathRoot()`. No class here keeps a root of its own; every inverse
   gets it from `applyPropagatingRoot()`.

9. **THE PROGRAM IS STORED VERBATIM, AS TEXT** (A4, GATES Q1.2):
   `<SSamplerProgram …><program><![CDATA[<NassauDigitalProgram …>…
   </NassauDigitalProgram>]]></program></SSamplerProgram>`. Character data,
   not nested elements, because the project file is read with QDom and QDom
   writes attributes SORTED BY NAME (measured): a program handed back through
   QDom would come back with its unknown attributes reordered, which the
   core's structural equality calls a different program. The core escapes
   `>` in everything it writes, so `]]>` cannot occur; the writer splits the
   section anyway if a kept text ever carries one.

10. **A PROGRAM THE CORE REFUSES ON LOAD IS KEPT, NOT LOST.** A value this
    core rejects (a newer build's polyphony, say) leaves the object
    `!isReadable()`: the text is written back on save character for character,
    the instrument plays the empty program, every edit is refused with the
    reason, and the load says so once. Discarding it would destroy the user's
    program on the next save.

11. **THE SELECTION IS MODEL STATE OF THE OBJECT, NOT OF THE PROGRAM**:
    `select-zones` is undoable (GATES Q1.1), changes no sound, publishes
    nothing, and is NOT saved. `selectedZones()` reports only ids that exist,
    in selection order, so a removed zone drops out and comes back with its
    undo. ASSUMPTION awaiting the author (proposal 56 §11 Q1).

12. **A `file:` LAYER IS FED BY GENERATIONS** (DESIGN §6.1 rule 1). After
    every committed edit and at bind time, `reconcileSamples_()` compares each
    layer's `src` with what is installed and what is on its way: a new
    `file:<path>` starts a JOB with the layer's next generation number (a
    per-layer counter that never goes back, so an undo that writes the old
    `src` back is a NEW generation); a `link:<n>` is fed by bounces
    (invariants 17–20); any other `src` (`test:…`, empty) REMOVES the layer's buffer (installed as a
    change too); a removed layer takes its buffer and its job with it; a job
    whose `src` was edited again before it finished is CANCELLED and its
    result never installed. The path resolves as the MIDI-file verbs resolve
    an input: absolute; next to the action script (`sampleBaseDir`); next to
    the project file (`SFilePathRef`); the cwd. ASSUMPTION (Q2): the stored
    `src` is the text the verb was given, not re-encoded relative to the
    project on save (relocation and "collect files" are not Q2).

13. **A JOB TOUCHES NO QOBJECT AND NEVER CALLS BACK** (THREADING rule 1: no
    signal, no queued call, no event from the worker; `twSampleSource` itself
    takes a `QString` and logs through `qWarning`, exactly as it does on the
    revalidator pool's `std::thread`s for the feel-flow bounce). One
    `std::thread` per job decodes with `twSampleSource` into an immutable
    `twSamplerSample` (`fromPlanar`, stamped with the generation), stores it,
    sets an atomic `done` and notifies a condition variable. The MAIN thread
    finds it, by POLLING (a 10 ms `QTimer`, running only while a job exists —
    the pumpLocator pattern) or by WAITING on the condition variable
    (`settle()`). Every worker is joined on the main thread: at install, at
    the poll after a cancel, and in the destructor (which releases a held
    one). A decode that fails leaves the layer on its previous generation,
    announced (`generation N FAILED (…); the layer keeps generation M`).

14. **INSTALLING IS A STATE CHANGE, AND ONE INSTALL CARRIES EVERY FINISHED
    JOB** (DESIGN §6.1 rule 2, proposal 56 invariant 6). An install is ONE
    installer call: `future` = program + every current generation (the slot's
    next instance initializer: a rebuilt instance is born with them,
    invariant 3) and `now` = `setSample` for each changed layer, then
    `collectRetired()` — under the processor's mutex, never in `process()`
    (proposal 56 invariant 5; a buffer replaced by THIS install is still
    parked and is freed by the next one). Through the slot's
    `applyToInstances()`, so the param epoch moves and the path above the
    slot is staled. Gated by `assert-sampler-layer trackEpochAdvancedSince=`
    across an install with no edit in between (`qxa.sampler_q2_generations`).

15. **NOTHING IS INSTALLED WHILE AN OFFLINE RENDER RUNS** (DESIGN §6.1 rule
    3). The poll installs only when `SAppContext::isRenderingActive()` is
    false; a job that finishes during a render stays queued and is installed
    by the first poll after it. NOT GATED: a scripted render sleeps on the
    main thread, so no poll can run during one, and the barrier's settle
    leaves nothing outstanding when a render starts (proposal 56 §11 Q2).

16. **`settle()` IS THE BARRIER'S WAIT** (B8, proposal 56 invariant 7):
    synchronous on the calling (main) thread, bounded, waiting on the jobs'
    condition variables rather than the event loop, installing what
    finished (`force`, ignoring rule 15 — the render has not started) and
    reporting how many were outstanding, how long it took, and which job is
    late. The shell calls it from `startRender()` and logs the line GATES
    Q2.3 asserts (`shell/CONTRACT.md`); `wait-sampler-generations` calls it
    without a render.

17. **AN ASSET LINK IS AN `SLink` TO THE BODY, NAMED BY A LINK ID** (proposal
    56 invariant 9). `set-layer src="asset:<name>"` resolves the name once and
    hands the core `link:<n>`, `<n>` an id of THIS program object (minted from
    `nextLinkId`, never reused) whose record is an `SLink` child targeting the
    asset BODY. The src text alone therefore says which body a layer plays,
    and every text-carrying inverse (restore-sampler-values, the core's zone
    and layer XML in reinsert-zone / reinsert-layer) carries a link exactly as
    it carries a file — no inverse names an asset. A rename moves only the
    registry's name; the link holds the body (GATES Q3.4). A link is PINNED
    (the `SLink` exists, and is saved) only while a layer names it; one no
    layer names keeps a WEAK pointer to its body, so an undo can name it again
    but it holds nothing — a pin nobody uses kept a refcount cycle (asset →
    track → slot → program → asset) alive and `~SProject` cannot order a cycle
    (measured: a teardown SEGFAULT). The redo of a `set-layer` names the link
    its first apply minted. A literal `link:` src is refused on every forward
    verb, and `asset:` on `add-zone` / `add-layer` (ASSUMPTION: link with
    `set-layer` once the layer exists).

18. **ONLY THE ASSET'S OWN CONTENT EPOCH TRIGGERS A BOUNCE, DEBOUNCED**
    (DESIGN §6.2, GATES Q3.2). The program listens to
    `SProject::arrangementChanged` (after every action) and, per linked layer,
    compares a KEY — the asset content's root component (identity), its
    `contentEpochNow()`, the window (start, length) and the rate — with the key
    its newest generation (installed or on its way) was bounced from. Equal: no
    work beyond the compare. Different: the layer is marked DUE and a trailing
    debounce (re)starts; when it fires, every due layer is bounced. The
    interval is 150 ms (`SMARAGD_SAMPLER_BOUNCE_DEBOUNCE_MS` overrides it): a
    gesture's burst of actions (16–60 ms apart) costs one bounce, and a 1 s
    asset bounces in 1–11 ms on the dev box (measured, `sampler_q3_asset_link`),
    so the debounce, not the bounce, is the latency a player hears. A NEW link
    (or one an undo writes back) bounces at once, without the debounce; a link
    read from a file is DUE at bind, because a load keeps invalidating after it
    binds (measured: two bounces per load otherwise).

19. **THE BOUNCE IS A GENERATION JOB** (DESIGN §6.1). It runs on the job's
    `std::thread` like a decode and installs exactly as one (invariants
    13–16): the worker freezes the content component's pages over the window
    — the `SCut::buildCapture_` pattern, `freezePage` positions the component
    itself under its own cursor mutex, nothing else is touched — into an
    immutable `twSamplerSample` at the project rate (the first two channels).
    It renders the asset's CONTENT over the asset's window (`windowStep(0)`,
    `getDurationBlocking()`), never the body's own reader, which plays a
    capture rebuilt asynchronously that may be stale. A bounce is cancelled
    between pages when superseded. `rendering` (DESIGN §6.2's mark) is a job
    outstanding OR a bounce due; the previous generation plays meanwhile.

20. **AN OFFLINE RENDER HEARS THE ASSET AS IT IS** (GATES Q3.1). `settle()`
    and the shell's `settleBeforeRender()` first call `flushDueBounces()`,
    which starts every due bounce at once (the debounce cut short), and then
    wait for it like any outstanding generation; the barrier's line names it.
    The debounce never starts a bounce while an offline render runs.

21. **A LINK THAT WOULD BE A RENDER CYCLE IS REFUSED WHEN IT IS MADE**
    (DESIGN §6.2, GATES Q3.3, Q3.q1). `set-layer` refuses an asset from
    which the sampler's own track is reachable, by
    `ssampler::assetReachesTrack`: child and content links (what
    `sarrangements::reaches` walks) PLUS, for every lane passed, the asset
    links of the NassauDigital programs on its slots — a plugin chain is not a
    child link, so without that hop a cycle through a second sampler is
    invisible. Which programs a lane holds is objects/track's knowledge: the
    shell installs `ssampler::setLaneProgramsHook`. A cycle built AFTER the
    link is invariant 22's.
22. **AN EDIT THAT WOULD CLOSE A CYCLE AFTER THE LINK IS REFUSED, BY THE SAME
    WALK** (QBX-267; the author's ruling of 2026-11-01). The shell installs
    `ssampler::assetReachesTrack` as model's whole-graph walk
    (`sarrangements::setReachWalk`), so `sarrangements::reaches` — which
    every containment and placement verb asks — follows sampler asset links
    too: the check a link is made under and the check a later move is made
    under are ONE function. A verb that attaches X under Y asks "is Y
    reachable from X" before it mutates and refuses with the hops in its
    reason: `place-asset`, `reparent-track`, `move-clip`, `duplicate-clip`,
    `link-clip`, `dissolve-arrangement` (which asks about the graph it LEAVES:
    its several moves are pending edges, `sarrangements::PendingEdges`, that
    the walk follows as child links). Proposal 56 §11 "QBX-267" lists the
    verbs that cannot close one, and why. Without the shell's hook (a unit
    test) the walk is the plain child-link one.

23. **A STRETCH LAYER'S GENERATION IS ITS SOURCE STRETCHED TO THE PROJECT
    TEMPO** (DESIGN §5, §13; proposal 56 §4.8). A layer WANTS a stretch when
    its zone's `mode` is `tempo-loop` or `tempo-oneshot`, its `tempoMethod` is
    `stretch`, its `beats` > 0 and the project has a tempo
    (`SProject::getBPMTempo()`). Then its generation job decodes (or bounces)
    the source and stretches it through `twSamplerStretch` by
    s = (beats · 60 / bpm) / (loop frames / source rate), the loop being
    `[loopStart, loopEnd)` clamped into `[start, end)` as the core clamps it
    (without loop points `[start, end)`): uniform, so a lead-in and a tail are
    stretched alike. The buffer carries s as its time scale; the CORE scales
    the layer's positions by it (DESIGN §13, settled 2026-11-02), so the model
    keeps source frames and qbx writes no program attribute. At s == 1 exactly
    (the loop's own tempo) and with beats = 0 the source plays as it is. This
    is the one place qbx READS program attributes beyond `src` (the zone's mode
    and method, the layer's beats and positions): what to render ahead is the
    host's decision. Numbers are parsed with `QString::toDouble` (C locale;
    `strtod` reads "1.5" as 1 under a German LC_NUMERIC, measured).

24. **THE SOURCE IS KEPT, AND EVERY CHANGE OF THE WANT IS A NEW GENERATION**.
    A stretch layer's generation keeps its unstretched source (planar, with its
    content hash) beside it. `checkStretch_()` compares each layer's want (the
    mode / method / beats / positions / tempo above) with the want of its
    newest generation (on its way, else installed) after every committed edit,
    on `SProject::bpmTempoChanged` (set-tempo and its undo, the loader), and
    when an onset analysis finishes; a difference starts a RE-STRETCH from the
    kept source (no decode, no bounce; a linked layer keeps the key its source
    was bounced from), superseding a job on its way. A want that turns OFF is a
    generation too (the source, unstretched). A layer with no kept source
    (it did not want a stretch before) starts from its `src`.

25. **ONSETS: FROM THE SIDECAR STORE, ELSE AN ANALYSIS THIS OBJECT STARTS**
    (DESIGN §5: the vocoder places its transient keyframes from the source's
    onsets). The job's worker asks the sidecar store for the source's `onsets`
    aspect by content hash (params-agnostic, as twGrainSource does). With none,
    the generation is stretched on the vocoder's fixed keyframe grid and, once
    installed, `maybeAnalyze_()` starts an ANALYSIS job on the kept source: a
    `std::thread` (not the revalidator's analysis lane, which a render pauses
    and which queues behind unrelated work — Q2's reason for its jobs) that
    runs `twDetectOnsets` with SPlainWave's parameters and STORES the result in
    the sidecar store (when enabled), so the next decode finds it. Its
    completion (polled, like a job) records the onsets by content hash and runs
    `checkStretch_()`: a stretch layer whose generation used other keyframes
    (the fixed grid) is re-stretched with them — a new generation. An analysis
    that finds no onsets changes nothing (the fixed grid is what an empty list
    means) and makes no generation.

26. **THE BARRIER WAITS FOR GENERATIONS, NOT FOR ANALYSES.** `settle()` first
    acts on analyses that have FINISHED (their re-stretch is then outstanding
    and waited for), never on one still running: a render after a tempo change
    hears the stretch (Q4.q1, Q2.3's mechanism), and a render before the
    analysis completes hears the fixed grid. Every render is still one
    generation; which one depends on whether the analysis finished, exactly as
    DESIGN §5 orders it ("rendered again when the tempo changes and once more
    when the analysis finishes"). `waitAnalyses()` (and
    `wait-sampler-generations analysis="1"`) is the bounded wait that a case
    uses to place itself after the completion.

27. **THE STRETCH IS DETERMINISTIC.** `twSamplerStretch` runs
    `twPagedVocoder::warpOffline` (single-threaded, double math, fft 2048, hop
    512, pitch ratio 1 — the twGrainSource configuration) over the whole
    source into exactly llround(frames · s) frames; the same source, factor and
    onsets give the same bytes in every process (gated against goldens). No
    stretch runs on the audio thread: the worker renders, the main thread
    installs (invariants 13–15).

28. **LIVE PLAY IS THE LIVE LANE'S, AND AN INSTALL NEVER TOUCHES A SOUNDING
    VOICE** (proposal 56 §4.9, Q5; DESIGN §6.1 rule 4, §9.4). An armed (or
    monitoring) NassauDigital track is played by the LiveGraphPump like any
    instrument: the live event source is collected inside the slot
    processor's render (plugins inv. 46), the block is the device's, and the
    rate refusal is the lane's (playback inv. 18). Nothing here or in
    tw/sampler knows whether a block is live or frozen, and nothing may start
    to: a sampler-specific live path would be a second latency to keep equal
    to nassau-analogue's. While the chain is live-owned a generation install
    still goes through `applyToInstances` (under the mutex the pump's render
    holds, so it lands between two live blocks) and stales the track as
    always (invariant 14); the live lane's exclusion keeps that stale from
    becoming a freeze of the live-owned processor. A voice already sounding
    keeps the buffer it started on (the core parks the replaced one, invariant
    5 of proposal 56); the next note plays the new generation. Neither the
    install nor `setSample` may reset the instance or drop a swap while a
    voice sounds.

### Q6 (QBX-272): recording into the instrument

Unnumbered here, because the Q5 and Q8 changes add invariants
in parallel; they are renumbered when those land.

- **THE VIEW, THE SELECTED PAD AND THE RECORD INPUT ARE STATE OF THIS OBJECT,
  NOT OF THE PROGRAM** (the author's answer of 2026-11-02, nassau-digital
  DESIGN §13). `view` (Key by default), `padBank` (0..5), `pad` (0..15) and
  `recordInput` (`audio:<device>:<mask>` or empty) are saved as attributes of
  `<SSamplerProgram>`, each only when not the default — every earlier file is
  byte-unchanged — and never in the program text, so an exported program never
  carries them. A pad is key 36 + 16·bank + pad; a pad whose key would pass 127
  does not exist and is never stored. A value this build cannot take on load
  is announced and the default kept. Changed only by `set-sampler-view` and
  `set-sampler-record-input`: undoable, absolute inverses, no sound, nothing
  published; both create the program object (as add-zone does), a refused one
  creates none.
- **THE LAST KEY PLAYED IS LIVE AND COMES FROM THE DEVICE TAP**: `lastKey()`
  is the last note-on (velocity > 0) the instance's own sink of its track's MIDI
  input port delivered — the shell's `SSamplerKeyTaps`, the tap Q7.2 reads too,
  never the engine's event feed, so a sequenced note does not move it. -1 until
  a key is played; not saved, not undoable.
- **WHERE A RECORDING LANDS IS DECIDED ONCE** (DESIGN §7). `place-sampler-
  recording` resolves the target (`target`, else the view: Pad → pad, any other
  view → key) and the key at its first apply, builds its children — add-zone,
  add-layer or set-layer for the program, `create-file-asset` (through the
  registry) for the asset — and HOLDS them, so the redo re-applies the same
  children: the same zone and layer ids, the same asset name. Pad: the pad's
  zone is the first zone with keyLo = keyHi = the pad's key; when it exists its
  first layer is replaced (src, start, end, loop points reset, beats 0; every
  other setting kept), else a one-key zone rooted there. Key on an empty
  program: a zone over the whole keyboard at the core's default root (60). Key
  on a program with zones: a one-key zone rooted on the last key played;
  REFUSED when none has been played. A refusal changes nothing: the file is
  pre-flighted before any child applies.
- **THE RECORDING IS A FILE LAYER AND AN ASSET** (Q-4). The layer's src is
  `file:<path>` (the capture's absolute path; fed by generations as every file
  layer, invariant 12), start/end = the kept span. The asset is a separate
  registered body (an `SCut` over the file), not a link the layer plays: the
  layer never bounces it. One undo removes both.

## Value syntax (the front end's, DESIGN §2.1)

On a numeric parameter a leading `+`/`-` is a RELATIVE edit; on an enum or a
bool the core refuses it. A negative ABSOLUTE value is written with a leading
`=` (`pan="=-0.5"`) — ASSUMPTION awaiting the author, since DESIGN makes every
signed value relative and names no absolute spelling. A text parameter
(`src`, `name`) is always literal. `add-zone` / `add-layer` values are always
absolute (there is nothing to be relative to).

## Persistence and binding, step by step

- **Save**: the slot writes an `<SLink>` per model child (its existing loop);
  the program, a QObject child of the project like every `SObject`, writes its
  own top-level element.
- **Asset links** (Q3): `<SSamplerProgram … assetLinks='<n>=<objectId>;…'
  nextLinkId='…'>` and one `<SLink objectId=…>` child per link a layer names,
  after `<program>`. Both attributes are written only when a layer names a
  link, so every Q1/Q2 file is byte-unchanged. The element kind is CONTAINER:
  a body the file lost costs its link (the layer then plays nothing,
  announced), never the program. The loader builds the asset body first (the
  `<SLink>` children order it); a track references its slots by attribute,
  not by `<SLink>`, so a program's links cannot close a loader cycle.
- **Load**: the loader builds `<SSamplerProgram>` first (it has no links), then
  the slot, whose `instantiateFromDomElement` now attaches its `<SLink>`
  children and hands each to `SPluginSlot`'s child-adopted hook. The shell's
  hook binds an `SSamplerProgram` under a NassauDigital slot: the initializer
  is set before the track declares its width, so the instance is born with
  the program. A child the file lost costs only its link: `SPluginSlot` is a
  CONTAINER element kind since Q1.

## Gates

- `qxa.sampler_q1_zone_id` (QBX-265) — an explicit zone id: accepted on an
  empty program (`nextId` past zone and layer), undo leaves `nextId`, the
  undone id is then refused as used-before, redo restores the same ids; a
  plain add after it gets `nextId`; refusals of a taken zone id, a taken
  LAYER id, a skipped-over id, a non-positive id and a layer `id`, each with
  the line only that refusal writes and the program unchanged; `id == nextId`
  and a jump. Sabotages that bite: no used-before check; no in-use check
  (the log assertions alone see it: the id then falls to the used-before
  refusal); `nextId` not moved past; a redo that adds afresh.

- `qxa.sampler_q1_verbs` — Q1.1 (each verb, one undo, the core's equality, a
  `differsFromCaptured` before every undo so the equality is not vacuous),
  Q1.q1 (ALL clamps per zone, undo restores each absolute value; relative edit
  on an enum and a bool refused), Q1.q2 (new id after an undone add), the
  refusals, the reinsert validation, and `installed="1"` before and after a
  2 -> 1 channel rebuild.
- `qxa.sampler_q1_persist` — Q1.2 first half: save, reload, structurally equal
  with no exception, a second save/reload a fixed point, bound after the
  reload, selection not saved, edit + undo after the reload.
- `qxa.sampler_q1_newer_program` — Q1.2 second half: a `version="2"` program
  with unknown attributes (in reverse alphabetical order) and elements at all
  three levels keeps them, in order, through a load, an edit, a save and a
  reload; a program this core refuses is kept verbatim; a lost program costs
  only its link.
- `action_roundtrip_test` — every verb and inverse, children included.
- `qxa.sampler_q2_generations` — Q2.1 (a swap's install advances the track's
  content epoch with no edit in between; the render after it is the new
  sample's single-generation golden byte for byte, and B's level from the
  onset), Q2.2 (each of two renders with a swap between them equals its own
  sample's single-generation golden), undo and redo as new generations, a
  2 -> 1 -> 2 rebuild born with the generation, a non-file `src` removing
  the buffer. A's golden IS `sampler_q0_note.wav`: the model's file layer
  renders Q0's test install byte for byte.
- `qxa.sampler_q2_barrier` — Q2.3 (a held job; the render waits, logs the
  line once, and equals the render made once the layer was ready), Q2.q3
  (a job held past a 300 ms bound refuses the render with the reason).
- `qxa.sampler_remove_plugin_undo` (QBX-254) — `remove-plugin` + undo, and
  remove + undo + redo + undo, bring the slot back with THE SAME program
  object: equal to the program before the removal, `installed="1"`, the layer
  still on generation 1 with one install (the same buffer, not decoded
  again), and a render byte-identical to the one before the removal. The
  mechanism is generic and lives in objects/track (its CONTRACT, model
  children): the inverse carries the slot's children pinned and re-adopts
  them, so the shell's child-adopted hook binds the program to the new slot
  before its instances are built. Sabotages that bite: no re-adopt; no pin
  (with `drain-pending-deletes` after each removal); a pin the first undo
  releases (bites the redo leg only); adopting after the link is parented
  (bites the render identity only: the instances are born without the
  sample buffers).
- Sabotages that bite (QBX-263): an install that does not reach the live
  instances; one that bypasses `applyToInstances` (no stale); a barrier that
  does not settle; a settle that does not install; a bound that does not
  refuse; the barrier line written when nothing was outstanding.

- `qxa.sampler_q3_asset_link` (QBX-266) — Q3.1: an edit inside the linked
  arrangement is a new generation, heard by the next render (rms 0.5 -> 0.25
  -> 0.5, two-sided bands), both by the debounce (`rendering="1"` right after
  the edit, `renders` up after a wait with no settle) and by an immediate
  render (the barrier starts the due bounce: its line, generation 3); Q3.2:
  three actions that miss the asset (a master lane, a note on the sampler's
  own clip, a clip trim in ANOTHER arrangement) leave `renders` and the
  generation unchanged, after the debounce and after a settle; Q3.4:
  `rename-asset` changes `asset=` and nothing else (no bounce), and after a
  save and a reload the link names the renamed asset, bounces it, and an edit
  of it after the reload is heard; and a bounce held by the render gate is
  SUPERSEDED by a second edit (cancelled, never installed: installs 2 -> 3
  with generation 4, the render hears the second edit).
- `qxa.sampler_q3_link_cycle` — Q3.3: refused for an asset over the master
  root and for one over an arrangement holding the sampler's track, each with
  its line, the src and the generation state unchanged; Q3.q1: refused through
  a second sampler (the message names the hop), accepted once that sampler
  lets go; positive controls (an unknown asset refused as unknown, a link that
  is no cycle accepted), the link's undo (buffer gone) and redo (same
  `link:1`, a new generation), and a literal `link:` refused.
- Sabotages that bite (QBX-266): see proposal 56 §11 Q3.
- `qxa.sampler_late_cycle_place_asset`, `…_move_clip`, `…_duplicate_clip`
  (duplicate-clip and link-clip), `…_reparent_track`, `…_dissolve`,
  `…_dissolve_replay` (QBX-267, invariant 22) — each verb refused for the
  cycle it would close after the link, with the line only that refusal
  writes and the tree unchanged; a positive control where the same edit
  closes nothing, with undo and redo; the refused edit accepted once the
  sampler lets go of the asset; `place-asset` also through a SECOND sampler
  (both hops named); `dissolve-arrangement` also for two moves that close a
  cycle only together, and for a plan that names the right folder only once
  the teardown and the earlier moves have run. Sabotages: proposal 56 §11
  "QBX-267".

- `qxa.sampler_q4_stretch` (QBX-268) — Q4.1: tests/sampler_q4_loop.wav (2
  beats at 120 BPM, a 1 kHz carrier under four hits, the first accented), zone
  tempo-loop by stretch, beats 2. At 120 BPM (factor 1, unstretched) the period
  is 1000.0000 ms (the measurement's positive control); `set-tempo 100` is a
  new generation (`timeScale="1.2"`), whose render measures a period of
  1200.0000 ms (± 1 ms; autocorrelation over 1150–1250 ms) and a carrier of
  999.772 / 1000.39 Hz (−0.39 / +0.67 cent; the band is ± 5 cent). Loop points
  in source frames ([12000, 48000), beats 1.5) are a new generation of the same
  factor, period 900.0000 ms: the core scales them. `tempo-oneshot` plays the
  stretched buffer once (1.2 s, then silence).
- `qxa.sampler_q4_generations` — Q4.2: from a cold sidecar store, with the
  analysis HELD, `set-tempo 100` is generation 3 on the fixed grid
  (`onsets="-1"`) and its render equals `goldens/sampler_q4_tempo.wav`; the
  released analysis (4 onsets) is generation 4 and its render equals
  `goldens/sampler_q4_onsets.wav` and differs from the fixed grid's; both
  goldens are an earlier run's renders, so each run compares across processes.
  Q4.q1: the stretch job held 1500 ms makes the render issued straight after
  `set-tempo` log the barrier's line once. Also: the analysis stored the
  sidecar (4 records); 80 BPM and its undo are generations 5 and 6 with the
  onsets at once, and the undo's render equals generation 4's bytes; back at
  120 BPM generation 7 is unstretched.
- Sabotages that bite (QBX-268): see proposal 56 §11 "Q4 as built".

- `qxa.sampler_q6_record` (QBX-272) — GATES Q6.1: record-start, stop, and
  the take lands as a whole-keyboard zone; the next note plays it at
  0.999967 / 0.999945 of the recording's RMS (left / right; the band is
  ± 2 %), 0.49997 / 0.24998 absolute; ONE undo restores the program captured
  before the press and removes the asset, the render is silent; the redo puts
  the same zone and asset back and renders the same bytes; the asset places on
  a lane.
- `qxa.sampler_q6_land_pad`, `…_land_key_empty`, `…_land_key_zones` — GATES
  Q6.2, one case each: the selected pad's key (bank 1 pad 5 = key 57, a new
  one-key zone; a second take REPLACES its layer 1, same ids, nextId unmoved;
  the undo of each); the whole keyboard on an empty program, played
  chromatically (480 / 960 / 240 Hz at the root and an octave either side);
  the LAST of two injected keys (67, not 64) on a program with zones, the drum
  map untouched, and the refusal when no key has been played.
- `qxa.sampler_q6_view_verbs` — the state verbs' undo, redo and refusals, the
  last key from injected MIDI (velocity 0 and note-offs ignored, a channel the
  input does not take ignored), place-sampler-recording driven directly
  (refusals, the view's pad, an explicit key target), save and reload (the
  state survives, the last key does not, the program text holds none of it).
- Sabotages that bite (QBX-272): proposal 56 §11 "Q6 as built".

## Not gated / known limits

- Q6: the CYCLE and PUNCH regions do not apply to a take into the instrument:
  the whole kept capture lands as one recording. Input MONITORING of the
  instance's record input is not built (the live monitor monitors track
  inputs). The capture opens the device the FIRST armed track names; a press
  arming tracks on different input devices records them all from that one, as
  for lanes. A layer src is the capture's absolute path (relocation is not
  Q6's, as in Q2). A pad cannot be reassigned to another key yet (DESIGN §7
  allows it; no verb does it), and the core's zone `pad` attribute is not set.
  A new instance starts in the Key view, not the per-user default (Q7.1).

- `qxa.sampler_q5_live_play` (QBX-271) — Q5.1, transport stopped: the
  sampler (panned left) and the test sine (panned right) armed on one MIDI
  input, one injected note-on. The sampler's onset lag 4646–4936 frames over
  25 runs against the live lane's bound of 7168 (4 ring + 2 lead + 1 device
  block; the author's answer of 2026-11-03 to what GATES Q5.1's
  "block-plus-lead bound" is); the sine's, from the same note, one frame later in every run,
  asserted as `refChannel` difference <= 64; each channel its own instrument
  at its closed-form level; `liveOwnedRefusals` 0.
- `qxa.sampler_q5_live_playing` — Q5.1 with the transport running (Monitor
  ON, armed after Play): onset lag 2324–2774 frames; a generation installed
  while the chain is live-owned and the transport runs; the sounding voice
  keeps its buffer; `liveOwnedRefusals` 0 — the case in which that bound can
  fail (a live lane without the exclusion counts 8 here, 2 without the
  install, and 0 in the stopped cases).
- `qxa.sampler_q5_live_generation` — Q5.q1: a generation installed under a
  sounding live voice leaves that voice on the old buffer (0.499971 /
  0.249976, the sine's balance) and the next note plays the new one
  (0.230373 / 0.230373, the saw's).
- Sabotages that bite (QBX-271): see proposal 56 §11 "Q5 as built".

## Not gated / known limits

- Q5.2 (the sampler's live onset against nassau-analogue's on the author's
  Windows 11 machine and USB keyboard) is a hand check by the author, never a
  gate; the procedure is in proposal 56 §11 "Q5 as built". Headless, the
  device block is the capture backend's fixed 1024 frames, so no case shows
  the latency at ASIO's 256 or 128; and the capture backend has no device
  rate other than the project's, so the lane's rate refusal is not exercised
  with a sampler (it is the lane's, gated for every instrument).

- Q4: a stretch DURING PLAYBACK (a tempo change while playing) installs like
  any generation and stales the track; no case plays. An analysis during a
  render is not waited for (invariant 26) and no case races one. The loop's
  SEAM is stretched as part of the whole buffer, not circularly: the vocoder
  sees no material across the wrap, so a seam can carry a small discontinuity
  the source did not have (not measured as a click; the period and pitch gates
  are unaffected). `tune`, master tune and LFO pitch still move the read rate
  in tempo modes (DESIGN §13, accepted for now), so a stretch zone with
  tune ≠ 0 drifts off the tempo. A kept source doubles a stretch layer's
  memory (the source beside its generation).

- A render cycle made AFTER the link is refused since QBX-267 (invariant
  22), by the verbs that can build one. What still is not: a project FILE
  that already holds such a cycle (hand-edited, or saved by a build before
  QBX-267) loads as it is; and the walk follows containment, content and
  sampler links but not ROUTING (a return or sidechain tapping a lane), the
  link-time check's limit too.
- A link no layer names holds its body weakly: `remove-asset`, an undo of
  the unlink and an undo of the removal bring back a NEW body, which the old
  link does not name; the layer then plays nothing, announced.
- The bounce reads the asset's window as start + length in the content: a
  stretched, warped or looped asset window is bounced unstretched and
  unlooped (no qbx verb makes one today).
- A bounce of an asset whose content holds an ARMED instrument track freezes
  that track's live-owned processor directly: the freeze is refused and
  counted, and the bounce is silent for that span. Untested, filed as
  QBX-274 (found by QBX-271).
- A bounce during PLAYBACK stales the sampler's track (invariant 14) but no
  case plays.

- Rule 15 (no install during a render) — see the invariant: no scripted case
  can make a job finish during a render, so removing the check passes every
  gate (measured, QBX-263). Q2.q1 is therefore not gated.
- Q2.q2 (round-robin page independence through qbx's page renders) is not
  part of QBX-263.
- An install during PLAYBACK stales the track (rule 14). Gated only for a
  LIVE-OWNED track (`sampler_q5_live_playing`, QBX-271); an install on a
  playing track that is not armed, whose pages the readahead re-freezes, is
  still not played by any case.

- `sampler-test-install` (testkit, Q0) bypasses the model: it sets its own
  instance initializer, and the next model edit on the same slot replaces it.
  Do not mix the two in one case.
- `SSamplerProgram::edit()`'s copy is defensive: every core edit is already
  all-or-nothing, so no sabotage of the copy can show in a gate.
- An older build (Q0 or earlier) opening a Q1 project does not know
  `<SSamplerProgram>`, so the slot's link to it dangles — and in that build
  `SPluginSlot` is still a PLAIN element kind, so it drops the slot too. A Q1
  or later build in the same position (a child it cannot load) keeps the
  slot.
