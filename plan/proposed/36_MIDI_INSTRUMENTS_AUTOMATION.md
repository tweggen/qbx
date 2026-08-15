# Proposal 36 — Event clips, instruments, MIDI output and automation

> **Status: DRAFT v2.2 (2026-08-15).** v2.2 adds §3.2.1 (events bubble up the
> track hierarchy) and the per-track MIDI-out delay (D6) from the requester's
> challenges; the requester accepted §11's decisions 1–6.
> v2 applies an adversarial review of v1
> (§13: 5 blockers, 16 majors, 13 minors — every one answered inline, so the
> reasoning survives); v2.1 applies the reviewer's verification pass (§13.2:
> pre-roll must reach back to the held note's start, one pinned `twEvent`, the
> barrier must explicitly forget processor continuity, beat-timebase links carry
> exact ticks, and a handful of AC strings). The blockers were real: the run barrier as written never
> reached the pages that are actually served; two phases defined two different
> `twEvent` types; the instrument event feed had no legal layering path; the MIDI
> cut window was specified in frames while every tempo consequence needed ticks;
> and half the early gates asserted on a log file that a `--test-case` run does
> not write. Design + phased plan; no code. Execution companion:
> `36_ORCHESTRATION.md` — one Opus 5 sub-agent per phase, every gate written as
> acceptance criteria.
>
> Written in a dedicated worktree (`docs/midi-instruments-automation`) from five
> parallel concept studies (engine, app model, plugin ABI + OS MIDI, UI, industry
> survey of REAPER / Logic / Cubase / Studio One / Live / Bitwig / Ardour) and the
> review.
>
> **Supersedes proposal 12** (`12_EVENT_TIMELINES.md`, design-only, 2026-06-15).
> 12's verdict stands — *one backbone, two lane kinds (event stream / control
> curve), three attach scopes, tempo as the coordinate system rather than a peer
> lane* — but 12 predates the page-frozen dataflow (19), plugin hosting (08),
> exact position domains (18), take stacks (17) and scoped invalidation (15).
> Every mechanism below is re-derived against the 2026-08 engine; where 12 said
> "the tw303a synth is the natural first consumer", §5.6 says why that is moot and
> what replaces it.
>
> Prerequisite reading: `docs/contracts/{FREEZE_PROTOCOL,CLIP_MODEL,
> POSITION_DOMAINS,THREADING}.md`, `plan/proposed/19_ASYNC_FREEZE_MODEL.md`
> ("Phase 2 REVISED", execution classes), `08_PLUGIN_HOSTING.md` (§Layer 1 ABI,
> §8 "Instrument plugins — gated"), `smaragd/tw303a/plugins/CONTRACT.md`,
> `smaragd/main/objects/track/CONTRACT.md`, `34_LEVEL_METERS.md` (the
> "read by position, never at freeze time" lesson — reused twice here),
> `35_MULTICHANNEL_SIGNAL_FLOW.md` (on `feat/multichannel`; §9.1 below is a
> hard sequencing dependency).

---

## 0. Scope

Four capabilities, one architecture:

| # | Capability | Industry anchor |
|---|---|---|
| A | **Event clips** — MIDI notes/CC/bend/pressure/PC/sysex plus *metadata events* (key/time signature, lyrics, chord symbols, articulations, string/fret hints, tracker columns) placed, moved, split, trimmed, looped, stretched, comped and undone exactly like audio clips | REAPER MIDI item, Logic MIDI region, Cubase MIDI part, Studio One instrument part |
| B | **Instruments** — CLAP / VST3 / AU plugins with event input become a track's audio *content*, rendered through the frozen-page path, saved like effects, plus in-repo instruments for deterministic gates | REAPER "instrument = FX slot", Cubase/Logic/S1 instrument track |
| C | **MIDI output** — a track's events sent to a physical or virtual MIDI port with channel remap, chase, panic and latency alignment | REAPER track MIDI hardware output, Cubase MIDI track output, Logic External Instrument |
| D | **Automation** — volume / mute (pan when the sink is stereo) / plugin parameters, as **track lanes in timeline time** and **clip envelopes in clip time**, modes Off / Trim-Read / Read / Touch / Latch / Write, sample-accurate gain, sample-offset plugin parameter events | REAPER track + take envelopes, Logic track/region automation, Cubase "automation follows events", S1 part automation |

Not in scope (named so nothing is assumed): live MIDI input / monitoring /
recording (proposal 21's live lane — §9.2 reserves the interface and P8 outlines
it), tempo *map segments* and ramps (a constant-tempo object lands here; segments
are proposal 37), multi-output instruments to return tracks (designed in §5.4,
built in P9), MIDI-FX event routing between slots (arp → instrument in-app; P9),
score / tab / tracker *editors* (the data model carries their events from P1; the
views are P9), MIDI clock / MTC, MPE editing UI, external-instrument audio return,
plugin-delay compensation.

---

## 1. What is true today (the facts the design is built on)

Every claim was verified in the worktree; the file:line anchors live in the five
study reports and the review, and are repeated here only where they decide
something.

**Engine**
- F1. `internalState`/`previousPage` chaining is almost unused for DSP state (only
  `twTrackMix::playOffset_` and `twSampleReader` position). Grain/vocoder are
  position-addressed pure functions. **The only cross-page-stateful thing in the
  graph is the plugin processor**, and it carries continuity *instance-side*
  (`lastEnd_/haveLastEnd_`, `twpluginslotproc.h:186-189`): a page not starting at
  `lastEnd_` ⇒ `resetInstances_nolock()` — no pre-roll, no chase, no per-page state
  (`twpluginslotproc.cc:392-399`); tap `reset()` is a deliberate no-op
  (`twplugininsert.cc:96-104`). Instruments join *that* class; its rules, not
  FREEZE_PROTOCOL's contiguity rule, are the ones to extend.
- F2. The scheduler's predecessor edge exists only while the previous node is in
  flight (`capture_revalidator.cc:369-383, 539-540`). Sequential consumers work
  because they *are* sequential; nothing guarantees cross-run coherence.
- F3. Render floor-aligns pages from the range start (`render_session.cc:182-183`);
  the readahead floor-aligns from the playhead and after a jump **reuses cached
  current pages ahead of it** (`audio_engine.cc:599-616, 645-655`). Page k from run
  A and page k+1 from run B can be served back to back. Inaudible for a reverb tail;
  for a held synth voice it is a dropped note ("run coherence", D4).
- F4. The byte-`cmp` gate is render-vs-render in fresh processes; render vs
  playback were never bit-identical for stateful inserts. Latent: a tap's cached
  playback pages can be spliced into a later render in the same process.
- F5. Plugin parameter edits are not position-deterministic: `setParam` → ring →
  `header.time = 0` of whichever chunk runs next (`twclapplugin.cc:592-637`; VST3
  `addPoint(0, …)`, `twvst3plugin.cc:754`). No transport/position reaches any
  plugin (`steady_time = -1`, `transport = nullptr`, `processContext = nullptr`).
- F6. **Track gain is a per-page scalar applied PRE-FX** in
  `twTrackMix::freezePage_nolock` (`twtrackmix.cc:542-548`) and `setTrackGain`
  bumps the whole component epoch. Mute is the parent nulling the plug. Pan does
  not exist (mono sink). The chain per bus is `twTrackMix → twPluginChain → twRewire`
  (`strack.cpp:380-386`).
- F7. Instrument shapes are rejected structurally: the processor's mapping table
  (`twpluginslotproc.cc:172-213`) has N→N, 1→1×N, 2→2-on-1-bus, else *Unsupported*
  — a 0-in plugin is transparent silence. `twPluginChain::rebuildWiring_nolock`
  already tolerates a 0-input head tap (`twpluginchain.cc:178-197`).
- F8. Anything plugin-related invalidates the **whole track**
  (`SPluginSlot::audioInvalidated → STrack::invalidateRenderPath()`); the range
  form exists and is what clip edits use (`strack.cpp:130-140, 200, 222`).
- F9. Range invalidation re-blesses non-intersecting pages
  (`twcomponent.cc:457-475`) — exact for position-deterministic components, only
  approximate past the range for a class-1 processor (a continuation page rendered
  from pre-edit state stays "current").
- F10. An epoch bump of the rendering component *from inside its render* livelocks
  the scheduler (`capture_revalidator.cc:441-513`, schedule/CONTRACT inv. 8) —
  invalidation is issued by consumers before demanding, never inside `pageFor()`.
- F11. `SApplication::pumpMeters` is the template for following the playhead
  without rendering (atomic position − latency, try-lock reads).
- F12. `twView::freezePage` with a null component returns an empty page and
  `twTrackMix::planPage` skips null resolutions — a non-audio SObject on a track is
  already harmless to the audio path.
- F13 *(review)*. **`invalidatePagesInRange_nolock` does not cascade** — it bumps
  only that component and re-blesses its own pages (`twcomponent.cc:457-475`);
  the readahead, the RT thread and the render decide "current" against the
  ROOT's epoch and cache (`audio_engine.cc:640-656`, `render_session.cc:191-208`),
  and every scheduler node's `freezePageWithInputs` cache-hits its own component's
  current page. **The only path that carries a change from a tap up to the root
  is the app-side `SObject::invalidateRenderPathRange` walk on the main thread**
  (`strack.cpp:130-140`, `sobject.cpp:855-873`). Anything that must re-render a
  path from a position onward has to be issued there.
- F14 *(review)*. The RT thread adopts a fresh current-epoch page mid-page as
  soon as it lands (`audio_engine.cc:373-390`, proposal 16). Re-staling pages the
  RT is currently serving = an audible switch at an arbitrary offset.
- F15 *(review)*. `tw/mix` and `tw/plugins` may not include each other
  (`tools/check_layering.py:30-52`); `twEditRange` lives in `tw/mix`.

**App model**
- M1. Q_PROPERTY inventory, whole app: `SObject{Solo, Muted, ArmedForRecording,
  Volume, Pan, Delay, SName}`, `SCut{Stretch, PitchCents}`. **Pan and Delay are
  dead** (setters emit, nobody listens). Only Volume and Muted reach the engine.
- M2. `SLink` = `{SObject&, startTime}`; serialized with no children and no id.
  Object ids in files are pointer values; there is **no format version attribute**
  on `<SProject>` (proposal 32 M0 is DRAFT). Tempo is one scalar `bpmTempo_`.
- M3. Only `move-clip`/`remove-clip` are placement-generic; `split/resize/duplicate/
  set-clip-name/take-*` are hard-coded to `SCut` (`ssplitclipaction.cpp:110` even
  string-compares the class name; `stakehelpers.cpp`, `sunsplitclipaction`,
  `sselecttakeaction`, `sremovetakeaction` cast too); `STakeStack` is a stack of
  `SCut`s. `SCut` cannot window non-audio content (its non-random-source branch is
  the *container* capture path).
- M4. Loader: flat top-level objects ordered by `<SLink objectId>` only; an unknown
  element name yields NULL and is skipped (two warnings, `sprojectloader.cpp:33-40,
  169-183`); every link that referenced it is unresolvable; **the leftover sweep
  drops everything at once** (`:198-243`) — the containing track, then the mixer,
  then the root is not found. A missing *sample* on a placed clip already kills a
  project the same way (`sample_missing_survives` keeps its bad cut unreferenced).
- M5. `STrack::trackChildWasAdded` inserts every duration-carrying child into every
  bus mixer keyed by `SLink*`; a null resolved component logs per freeze
  (`twview.cc:35`); `STakeStack` avoids that with a private silence component.
- M6. The `.qxa` runner and `action_roundtrip_test` are the only gates. The
  runner's verify-undo pass compares **track counts only** (`sactionrunner.cpp:
  230-296`); `--test-case` runs write **no log file** (`main.cpp:225-235`);
  `assert-meter` drives the LEGACY PULL, which gates on the *chain's* epoch that
  `invalidateRenderPath()` does not reach (CLAUDE.md "Level meters"). Existing
  verbs the ACs lean on: `assert-audio-energy/-frequency/-peak`,
  `dump-playback-capture`, `set-locator`, `expectReject`, `undo count=`.
  There is no byte-compare verb, no log verb, no `seek` verb.

**Plugin layer**
- P1. `twPlugin::process(in, out, n)` is audio-only; `twPluginIoLayout` is main-port
  channel counts only (aux outs are read by the CLAP/VST3 backends and discarded);
  `acceptsNotes()` and `twPluginDescriptor::isInstrument` exist, are derived, cached
  and serialized, and are unused. Tail is not queried. AU enumerates `aufx/aumf`
  only. Plugin output events are dropped; the VST3 host offers no `IEventList`,
  `IMidiMapping`, `INoteExpressionController`.
- P2. The processor knows the absolute page position (`renderPos_` via the `seekTo`
  side channel) — everything an event scheduler needs is already there; no backend
  forwards it. The legacy pull is positionless.
- P3. The in-house 303 exists only as unit generators (`twSaw`, `twMoog`,
  `twTestSeq`) and a dead demo graph (`tw303a.cc`); **the app never instantiates
  any of them**. There is no voice/envelope/note abstraction.
- P4 *(review)*. No existing golden combines a non-unity track fader with a
  plugin: `render_sawtooth_with_effects` has a folder fader and no plugin,
  `plugin_bypass_and_param` no fader, `meter_postfader` no plugin.

**UI**
- U1. Clip painting is already polymorphic (`STrackRendererInline` →
  `lk->getSObject().getInlineRenderer()->draw()`); the only leak is
  `SCutRendererInline`'s `container = !content.getRandomSource()` heuristic.
- U2. Proposal 09's tab shell was **never built** — the central widget is one
  `SStdMixerView` (3994 lines, the recorded largest-file debt). Docks that follow
  the selection are the house pattern (Track Detail, Clip Properties; timeline inv.
  8/9). Sub-lane geometry for automation lanes is prepared (`STrackRow::isSubLane()`
  with the comment "Automation lanes will join it under the same rule").
- U3. The ruler already draws bars.beats.ticks at 480 PPQ off `STimeGridSpec`
  (sec/beat, beats/bar); snapping has an unexposed subdivision; the tempo box
  (`smainwindow.cpp:1929`) and ruler "Set BPM" (`sstdmixerview.cpp:1554`) write
  the project directly, not through an action.

---

## 2. The eight decisions

Each is stated with the alternative that was rejected and why. They are settled
for execution; an implementation that finds a genuine contradiction stops and
writes it into §11 (orchestration ground rule 1).

### D1 — Events are model data, not pages
An event stream is an immutable, sorted, binary-searchable **snapshot per clip**
(the `SCutSnapshot` pattern), sliced by position by whoever consumes it. There is
no "event page" type, no event `planPage`, no scheduler edge for events. Rejected:
an event page kind parallel to `twOutputPage` — it would touch `tw/pages`,
`tw/graph`, `tw/schedule`, `twFrozenInputs`, `copyData`, buy nothing (events are
not the output of an upstream computation; a page of them is a cache of a copy)
and bake the "what is held at page start" question into every page instead of
answering it once as a query (`stateAt(P)`, §4.2). MIDI-effect components later =
a transform on the snapshot, not a page.

### D2 — Ticks in the content AND the window; frames on every track-facing side; one named conversion; MIDI placements follow the beat
`SMidiSequence` stores event times in **musical ticks (PPQ 960, integer)** — the
domain every reference DAW stores MIDI in (REAPER 960, Cubase 480, Logic 960,
Ardour 1920; Ardour ≤ 6 with samples + tempo map is the cautionary tale that
forced their 7.0 rewrite). **`SMidiCut`'s window is tick-native too** (`srcStart`,
`lengthTicks`, `loopTicks` as exact-rational ticks; `rate` a Fraction) — a musical
window over musical content, so a tempo change keeps the same notes inside the
clip. Every side the track sees — `getDuration()`, `mapTimelineToComponentPos`,
`SLink::startTime`, automation lanes, the whole engine — speaks **frames**,
derived through the single conversion `twTempoMap::ticksToFrames / framesToTicks`
(constant-tempo map now: `usPerQuarter` int64, exactly SMF's tempo unit, so the
conversion is an exact rational: `frames = ticks · usPerQuarter · srate / (ppq ·
10⁶)`; tempo *segments* later behind the same calls, proposal 37). `twTempoMap` is
the **single authority** for tempo: `SProject::bpmTempo_` becomes a derived view
(`set-tempo bpm=` stores `round(6e7/bpm)` µs/quarter and the ruler's
`STimeGridSpec` reads the map — today's `60/bpm` and the map would otherwise
disagree by ~0.1 µs). `tw/core/twdomains.h` gains a **`TickPos` exact-rational
domain** (a `Fraction` wrapper, mirroring `SCut`'s rational `srcStart`); content
events use integer ticks. Bonus: ticks are sample-rate-free — an `SMidiCut` needs
no `durationSec` migration and rebuilds on `sampleRateChanged` like on
`bpmTempoChanged`.

**Placement follows the beat by default for MIDI (review #13):** `SLink` gains
`timebase = time | beats` (default `beats` for links whose object is an event
clip, `time` for audio — REAPER's defaults). A `beats` link carries an **exact
`startTicks` (Fraction) as the authority** and derives `startTime` (frames)
through the map; `move-clip` / `duplicate-clip` / `place-*` on a `beats` link
convert the frame argument to ticks once and store ticks (so repeated tempo
edits never drift — a frames-only rescale would lose a frame per edit).
`set-tempo` re-derives `startTime` for every `beats` link, walking nested
containers and assets too, as part of the action (the inverse restores the map;
the ticks never changed) — so a MIDI clip at bar 5 stays at bar 5 and audio
does not move. This is REAPER "Beats (position, length, rate)".

The `SClipWindow` interface (D8) is phrased in the **window's own units** with a
frame-facing read API (`duration()`, `loopLength()` in frames — derived for MIDI)
and exact setters that take a **timeline frame** and convert once inside the
implementation (`SCut`: warped→source; `SMidiCut`: frames→ticks through the map).
Undo stays exact by LIFO: `set-tempo` is itself an action, so an inverse captured
in frames is re-applied at the same tempo it was captured under (the map is never
written outside the action system after P1). **Take stacks are homogeneous by
`contentKind()`** — an audio take and a MIDI take never share a stack
(`add-take` refuses; `select-take` therefore never moves a clip between the bus
mixers and the event clip set). Split/trim arithmetic converts the frame offset to
ticks through the map once (exact rational), like `SCut` converts warped→source
once.

Two of the three code studies recommended frames-now. Overruled on the industry
evidence: recorded MIDI that does not follow a tempo change is a defect users hit
in the first hour, and the migration cost of switching later is precisely
Ardour's. The cost we accept is bounded to `SMidiCut` + one link attribute.

### D3 — No track kind; an instrument is a slot with a role; clip kinds mix
REAPER's model: any track holds audio and event clips; the instrument is
`SPluginChain` slot 0 carrying `isInstrument`; MIDI-out is one serialized
attribute pair (`midiOutPort`, `midiOutChannel`). Rejected: an explicit
`kind = audio|instrument|midi` (Cubase/Logic) — a second source of truth undo can
desynchronize, kind rules for folders/edit groups/`move-clip`, and old projects
would need a default. Rules: at most one instrument per track, always slot 0
(`insert-plugin` of an instrument descriptor goes to slot 0; a second is refused —
one event feed per track, unlike REAPER's multiple VSTi; `reorder-plugin` cannot
move an effect before it); an event clip on a track without an instrument is
**inaudible, not rejected** (like a take stack with `activeTake = -1`); an audio
clip on an instrument track **sums** with the instrument (§4.3; every bus mixer
receives the same mono clip page today, so the sum is centre-panned mono into a
stereo instrument's outputs — acceptable, stated); the UI shows an instrument
glyph and colour, derived. Split of an event clip is **non-destructive**: the
window gates notes (a note straddling the split sounds in the head until the split
point via a synthesised note-off, and does not sound in the tail) — REAPER's
default; a content-editing `split-notes-at` is a later verb (review #10 —
content is shared, so a split must not edit it).

### D4 — Class-1 continuity: reset + chase + pre-roll; a RENDER barrier on the main thread; playback splices accepted
An instrument page frozen at a position that is not `lastEnd_` is a REPOSITION:
`reset()` (all notes off) → chase `stateAt(P − K)` (held notes with their
velocities, sustain, last CC values, bend, program) at offset 0 → pre-roll K frames
with events at their real offsets, output discarded → render the page.
**K reaches back to the held notes:** `K = min( max(4096, tailFrames(),
P − start(earliest note held at P)), 4 s )` — `stateAt` returns each held note's
start, so a pad held since 0 s and located at 2 s is pre-rolled from its own
note-on and its envelope arrives at P in the same state as in a continuous run
(a note held longer than 4 s has converged by then; the cap bounds cost). Chase
at `P − K` covers only notes started before that. Instruments need no upstream
pages for pre-roll, so it is plannable without a `planPage` override (plugins inv.
14). Effects stay reset-only. Rejected: capturing the plugin state blob per page —
`saveState()` per 65536-frame page per instance is a serialization of the *preset*,
not the voices (no format defines otherwise), MB-sized for a sampler, and VST3
`setState` is not RT-safe (F1, proposal 19's execution-class analysis).

**Run barrier — v2 (review #1, #6).** v1 invalidated "the taps" from
`AudioEngine::seekTo` and the readahead jump; F13 shows that reaches nothing the
consumers look at, and F14 shows a playback barrier would add a mid-page switch
(a click) on every seek. Therefore:
- The barrier is the **full path** invalidation, on the **main thread**:
  `STrack::invalidateRenderPathRange(pos, INT64_MAX)` for every track owning an
  **instrument** processor (registered via `STrack` → `SApplication`; effects are
  NOT barriered — their splice at page boundaries is today's behaviour) **plus an
  explicit `twPluginSlotProcessor::forgetContinuity()`** (reached through
  `SPluginSlot`) — an epoch bump does NOT clear `lastEnd_` (only `rebuild_nolock`
  and the rate checks do), and a render whose first page starts exactly at the
  previous run's `lastEnd_` would otherwise continue that run's voices.
- It is issued at **render start** (before the worker spawns — this is the
  determinism hole F4) and at **play start**, immediately before
  `twSpeaker::startOutput()` (which is what calls the engine's pre-readahead
  `seekTo(locator)` + `startReadahead()`, on the main thread) — that covers
  locate-while-stopped, since a stopped locate demands nothing until play.
- It is **not** issued on a seek during playback or on a loop wrap: those keep
  today's page-boundary splices (chase + pre-roll make a hole page approximately
  right; a mid-page RT switch would be worse). Stated as accepted and NOT gated.
- Idempotent under any ordering (THREADING rule 4): a late barrier costs one
  re-render, never a wrong page served as current; issued before demanding, never
  inside a render (F10).

Determinism claims, precisely: render = ONE run from the range start, forced by
the barrier even after in-process playback ⇒ byte-`cmp` across builds and
processes. Render vs playback are *not* bit-identical (different run starts) —
they never were for stateful inserts; every PR body says so. Gated: two renders
`cmp` equal (one after playback in the same process); a locate-then-play capture
case shows the chased note in the first page after the locate.

### D5 — Automation is a curve snapshot consumed at freeze time; the fader moves post-FX
A `twAutomationCurve` (immutable sorted breakpoints in frames, `valueAt`,
`fillRamp(dst, P, n)`) is set on the consuming component under its mutex and read
once per page into a local (THREADING rule 2). Consumers: a new **`twGainStage`**
between the last tap and the rewire (the fader, mute-with-ramp, later pan) with a
per-sample ramp — exact, 65536 multiplies; and the slot processor, which turns
per-parameter curves into per-4096-chunk sample-offset `ParamValue` events
(value-at-chunk-start "chase" first, then breakpoints, then a dense 64-frame ramp
along continuous segments for plugins that do not interpolate). **The epoch is the
hash**: every automation edit range-bumps the *consuming component* through the
app-side path walk (F13) — gain stage exact range; processor `[a, ∞)` because it
is class-1 (F9); an app-side edit that touches no component epoch is invisible to
the scheduler by construction (F8, F10).

The fader moves from `twTrackMix` (pre-FX, F6) to `twGainStage` (post-FX) for
**every** track: an instrument's output must be under the fader, and post-insert
faders are what every reference DAW does. Honest golden statement (review #7):
no existing golden combines a fader with a plugin (P4), so every existing render
is byte-identical *by construction* (the same multiply on the same page); the
ordering change itself is gated by a **new fixture with a closed form**
(`fader_post_fx.qxa`), not by a `cmp` — and because a linear insert cannot tell
pre- from post-FX, `tw.test.clap.gain` gains an optional **hard-clip threshold
parameter** (P2) so the fixture has an order-sensitive case: fader −6 dB then a
clipper at 0.5 leaves a unit-peak sawtooth unclipped pre-FX (RMS = 0.5·rms) and
clipped post-FX (lower, closed-form RMS). `twGainStage` implements the legacy pull
(`calcOutputTo`) too, so `assert-meter` keeps working — and it retires the
"legacy pull does not see a gain change" caveat (the rewire's producer is now the
gain stage, whose epoch `copyData` gates on). Mute: the `self:Muted` **lane**
drives the gain stage (audio, ramped); the mute **button** / `set-track-mute`
stays structural (parent plug-nulling, solo rules); with a Read `self:Muted` lane
present, `set-track-mute` writes a point at the locator, exactly like Volume.

Clip envelopes live on the **window object** in clip time (`SCut` gain envelope
applied to `childPage` before `mixFrom` — paying mix/CONTRACT's "per-clip gain not
modeled" debt — and travel with the cut across placements/takes/assets); placement-
scope (per-`SLink`) envelopes are deferred until proposal 32 gives links identity
(M2). Modes: Off / Trim-Read (default: static value + curve) / Read / Touch / Latch
/ Write; Touch/Latch/Write are UI recorders that commit ONE `set-automation-points`
per gesture (never an action per block). Plugin gestures
(`ParamGestureBegin/End` out) are the touch punch-in.

### D6 — MIDI out is emitted at PLAY time from a scheduler thread, never at freeze time
The metering lesson (34) applies verbatim: pages are frozen ~1.4 s ahead and by
renders with no playhead. A `MidiOutPump` (main-thread `QTimer`, **20 ms period,
250 ms lookahead window**, a de-dup cursor per track keyed `(clip key, event
ordinal, loop iteration)`) follows the playhead atomic (F11), slices the track's
event clip set by timeline position (D1 — the event data never enters the freeze
model for MIDI-out) and hands `{dueHostTimeNs, port, bytes}` to a
`MidiOutScheduler` std::thread (no Qt) through an SPSC ring; CoreMIDI/ALSA-seq
get driver timestamps handed off early, WinMM gets send-at-due-time (±1 ms,
honestly not gated). Alignment: `dueHostTime = hostTime(playhead) +
deviceOutputLatency − midiOutLatency − globalOffset − trackOffset`, reusing
`meterLatencyFrames()`'s device→project mapping. **`trackOffset` is a per-track,
signed, adjustable MIDI-out delay** (`STrack::midiOutOffsetMs`, ±500 ms,
serialized; `set-track-midi-output offsetMs=`; positive = send EARLIER, the
requester use case: outboard gear whose audio return arrives late is compensated
so its audio lands on the grid for monitoring or recording; a global default
lives in Options). Together with proposal 21's per-device *recording offset* it
aligns a full MIDI-out → outboard → audio-in round trip; the external-instrument
object that bundles the two is P9. On start/locate: chase CC/PC/
bend (always) and note-ons (per setting; default OFF for MIDI-out); on loop wrap:
the window is split at the cycle end, held notes get note-off at the cycle end,
chase re-issued at the cycle start; on stop / locate / panic: sustain-off + all-
notes-off per used channel. Renders emit nothing (no playhead). Backends behind a
`MidiOutput`/`MidiInput` interface in `tw/devices` mirroring `AudioBackend`/
`AudioInput`: WinMM, CoreMIDI, ALSA sequencer, **capture** (default under
`--test-case`, `SMARAGD_MIDI_BACKEND=capture`), null. **Measurement is
independent of the model under test** (review #12): the capture MIDI backend
records `{hostTimeNs, bytes}` only; the AUDIO capture backend records the host
time of every delivered block; the testkit maps host time → project frame through
that audio timeline (piecewise linear), so `assert-midi-out` measures the pump
against the audio clock, not against itself. `SMARAGD_CAPTURE_SPEED ≠ 1`
invalidates the mapping — MIDI-out cases run at 1.0. Rejected:
RtMidi/libremidi/PortMidi — legitimate, but the repo convention is hand-rolled
backends behind an interface, and none of them adds what WinMM lacks.

### D7 — In-repo instruments first: a native 303 and test CLAP/VST3 sine instruments
The instrument path (event delivery, chase, pre-roll, run barrier, byte-`cmp`
gates) is built and gated against instruments that need no SDK and no third-party
binary: `twNativeInstrument` (a monophonic 303 voice — saw/square, slide, decay
envelope → cutoff, ladder filter lifted from `twMoog` into buffer functions,
accent; `format="tw"`, registered like `twPassThrough`), then `tw.test.clap.sine`
+ `tw.test.clap.arp` in `twtestclap.c` and a split component/controller
`TestSine` in `twtestvst3.cpp` (closing plugins/CONTRACT's "split VST3 pair
untested" debt, which becomes load-bearing because `IMidiMapping` and note
expression live on the controller). Real third-party instruments are the
acceptance *demo*, never the gate. Byte-exact continuity gates use the
envelope-less sine fixtures (their steady state has no memory beyond the held-note
set); the 303 (envelope, filter memory) gates *presence* and *warmth* (energy in
the first frames after a locate), not bytes.

### D8 — Persistence tolerance and the window interface land first, alone
Two prerequisites are pure app-model work, independently gateable, and benefit
every future object type. (a) **Loader: prune-and-retry per element kind** — an
unresolvable `<SLink>` inside a *container* (track / mixer / take stack) drops the
LINK; a *window* object (`SCut`/`SMidiCut`) whose content link is dead is dropped
itself (and its own placement then falls under the first rule on the next pass);
iterate to a fixed point; the root must resolve or the load fails (M4 — today's
single sweep drops everything at once, and a placed clip with a missing sample
already kills a project). Plus `formatVersion` on `<SProject>`, readers warn but
never refuse. (b) **`SClipWindow`** — the window arithmetic `split/resize/
duplicate/take-*/unsplit/select-take/remove-take/set-clip-name` need — extracted
from `SCut` into `app/model` with a per-`contentKind` **wrap factory**
(`SClipWindow::wrapContent(SProject*, SObject& content)`), so those verbs dispatch
on the interface (every cast site in M3, including the class-name string compare)
and `STakeStack` becomes a stack of windows. Rejected: an "event mode" inside
`SCut` — it pushes `if (isEvent)` into every reader/capture/preview path of the
most-tested class in the repo.

---

## 3. The app model

### 3.1 Objects — content / window / placement, the house pattern applied

```
SMidiSequence : SObject            CONTENT  (the SPlainWave analogue)
  events_        sorted vector<SEvent>, times in TICKS (PPQ 960, integer)  D2
  ppq_ = 960, origin (smf | recorded | drawn), lengthTicks_
  snapshot()     shared_ptr<const SEventTable> — immutable, swapped under mutex()
  hasDuration()  = true (override; the base derives it from children)
  getDuration()  = lengthTicks mapped through the tempo map (frames)
  getRandomSource() = NULL; getRootComponent() = a private silence component (M5)
  contentKind()  = Event                                              (SObject virtual, P0a)
  persisted INLINE:  <SMidiSequence …><events count=…><e …/></events></SMidiSequence>
  durationSec migration: overridden (ticks are rate-free)

SMidiCut : SObject, SClipWindow    WINDOW   (the SCut analogue; NOT an SCut mode)
  content_ SLink → SMidiSequence (+1 ref, exactly SCut::content_)
  srcStart (TickPos, exact rational), lengthTicks (TickPos), loopTicks (TickPos), rate (Fraction)
  transpose (int semitones), velocityScale (double), channelOverride (-1 = keep)
  Q_PROPERTY Rate, Transpose, VelocityScale
  frame-facing (derived): getDuration(), loopLength(), startOffset()
  snapshot(): SMidiCutSnapshot { window…, shared_ptr<const twEventSeq> framesSeq }
              — the FRAME-domain event sequence with transpose/velocityScale/channel applied,
                rebuilt on any content/window/tempo/sample-rate edit
  mapTimelineToComponentPos: identity when looping, else + startOffset  (mirrors SCut::clipToReaderMap)
  listens to SProject::bpmTempoChanged / sampleRateChanged → rebuild snapshot, emit durationChanged

SLink                              PLACEMENT (+ `timebase` = time | beats; D2)
STakeStack                         a stack of SClipWindow, homogeneous by contentKind (D8b)
```

`SEvent` (model) is a fixed-shape typed record; unknown kinds round-trip verbatim
(the reader keeps the whole attribute map for an unknown `k`):

```
struct SEvent {
  int64      t;        // ticks, sequence-relative
  uint16     kind;     // Note, CC, PitchBend, ChannelPressure, PolyPressure, ProgramChange, SysEx,
                       // Tempo, TimeSig, KeySig, Marker, Lyric, ChordSymbol, Articulation,
                       // StringFret, TrackerCell, Text, NoteAttr; 0x8000+ = unknown/vendor
  uint8      channel;  // 0-15, 0xFF n/a
  int64      dur;      // Note only (ticks); notes are stored WITH duration, paired on export
  int32      a, b;     // note: key, velocity | cc: number, value | pb: value | pc: program …
  double     f;        // release velocity | tuning cents | …
  QString    text;     // lyric / chord / marker / articulation name
  QByteArray blob;     // sysex / unknown payload PRESERVED
  bool       muted;
};
```

Metadata events (score / tab / tracker) are ordinary kinds: an editor kind decides
what to render (§6.2); Logic/Cubase/S1 attach articulation *to the note* — ours is
an `Articulation` event at the note's tick + `text`, and `NoteAttr` references a
note by `(t, key, channel)` for a per-note attribute bag.

XML: one `<e>` per event, generic attribute names, `k` = a name for known kinds and
a hex number for unknown ones, sorted on write (diff stability, proposal 32).
Inline by default; an imported `.mid` is materialised inline on first save so
note data can never go missing the way a sample file can. SMF import with a
tempo *map*: ticks are preserved, the first tempo becomes `set-tempo` if the
project is empty, later tempo events are kept as `Tempo` metadata events and a
warning says timing is constant until proposal 37.

Assets / containers: a container capture renders its tracks' chains, instruments
included, so an event clip inside an asset's track is heard through that track's
instrument; an event clip placed directly on a container without an instrument is
silent (D3). Project end for a track with an instrument = last event clip end +
`tailFrames()` (deliverable P3b).

### 3.2 Track: no kind, three derived facts, two attributes
`STrack` gains `midiOutPort=''` / `midiOutChannel='-1'` / `midiOutOffsetMs='0'`
/ `midiRouting='auto'` (serialized) and four derived, non-serialized helpers:
`instrumentSlot()` (slot 0 with `isInstrument`), `hasEventClips()`,
`hasMidiOut()`, `eventFeed()` (§3.2.1). `trackChildWasAdded/Moved/Removed` and
`trackChildDurationChanged` route `contentKind() == Event` children into the
per-track **event clip set** (§4.2) keyed by `SLink*` — same slots, same key rule
(CLIP_MODEL "identity is the SLink pointer") — and NOT into the bus mixers, so no
dummy freeze per page per MIDI clip. Every event edit calls
`invalidateRenderPathRange(a, ∞)` on the cut (open-ended: the consumer is class-1,
F9) and that walk must reach the **instrument tap** via the processor's epoch
(the same "an `SPluginChain` is not an `SLink` child" pitfall as plugin edits;
`STrack::bumpRenderChainEpochRange` already covers the chain).

### 3.2.1 Event routing in the track hierarchy (folders) — events bubble up like audio
Audio from a child track is summed by its parent (folder tracks, assets). Events
do the same, under one rule (requester use case: a drum machine on the parent,
each child holding one pattern on the same MIDI channel):

- A track's **event feed** = its own event clip set **merged with the feeds of
  every child track that passes events up**. A child passes events up iff it has
  **no instrument slot and no MIDI-out port** — "consumed here, or bubbled up" —
  unless its serialized `midiRouting = auto | parent | none` says otherwise
  (`auto` is the rule above; `parent` forces bubbling even past a local
  consumer; `none` keeps them local). REAPER analogue: MIDI passes through a
  child's empty FX chain to the folder parent's input, so a VSTi on the folder
  plays the children; Cubase/Logic route MIDI tracks to instruments explicitly —
  `midiRouting=parent` is that explicit route restricted to the hierarchy.
- The feed is a `twEventMerge` (tw/events, a `twEventSource` over N sources):
  k-way merge by time; chase = union of the sources' `stateAt`; **note ids are
  namespaced per source** (source index in the high bits), so two children
  playing the same key on the same channel are two overlapping notes to the
  instrument, never one truncated one.
- **Mute / solo**: a child that is muted or solo-excluded (`app/model/
  ssolorules.h`, the same resolution the summing container uses for audio)
  contributes no events; the parent's own mute silences the parent's audio as
  today. Clip-level mute (`SLink`/take) is already inside the clip set.
- **Invalidation** needs nothing new: an event edit on a child calls
  `invalidateRenderPathRange(a, ∞)` on the cut and the existing ancestor walk
  (`STrack::bumpRenderChainEpochRange` at every ancestor track) reaches the
  parent's chain — i.e. the parent's instrument tap.
- The parent's **MIDI-out** sends its whole feed (children included); the
  parent's instrument consumes it; both at once is allowed (the feed is read,
  not moved). Containers/assets: same rule inside the container.

### 3.3 Automation persistence
`SAutomationLane` — a plain owner-held `QObject` (NOT an SObject; never an `SLink`
child): `{ParamRef target; Mode mode; sorted points {Fraction t; double v; Curve
c; double tension}; shared_ptr<const twAutomationCurve> snapshot}`. Owners and
their time domains:

| Owner | Lanes | Time | Travels with | XML |
|---|---|---|---|---|
| `STrack` | `self:Volume`, `self:Muted`, later `self:Pan` | timeline frames | nothing (arrangement time) | inline `<automation><lane target=… mode=…><p t= v= c=/>…</lane></automation>` |
| `SPluginSlot` (in `main/objects/track`) | `param:<id>` (+ `name` for recovery) | timeline frames | the slot (survives `reorder-plugin`; dies with `remove-plugin`, whose inverse carries it) | inline in `<SPluginSlot>` next to `<state>` |
| `SCut` / `SMidiCut` | `cut:Gain` (audio), `cut:VelocityScale`, `cut:Transpose` (event) | clip-relative frames | the window (every placement / take / asset of it) | inline in the cut element |
| `SLink` | — | — | — | **deferred** (no identity, M2) |

`ParamRef` spaces: `self:<Q_PROPERTY>` (Volume, Muted; Pan only after the sink is
stereo — M1), `param:<id>` on a slot (value in the plugin's host-facing domain —
normalized for VST3, plugins inv. 26 — the same domain `set-plugin-param` uses),
`cut:<prop>`. `Rate`/`Stretch` are not automatable (they change duration).

Older builds ignore inline `<automation>`/`<events>` children of known elements
(the loader orders on `<SLink>` children only) — that is why they are inline and
not top-level objects.

### 3.4 Actions (the scripting API grows; verbs are ABSOLUTE like `set-pitch`)

| Verb | Attributes (name = default) | Notes |
|---|---|---|
| `insert-midi-clip` | `trackPath`, `timePos`="0", `duration`="0" (0 = one bar), `name`="" | sequence + cut + link (`timebase=beats`); inverse = remove-clip |
| `import-midi-file` | `trackPath`="", `filePath`, `timePos`="0", `mode`="tracks\|channels\|merged", `newTracks`="1" | one sequence per SMF track; PPQ rescaled to 960; first SMF tempo → `set-tempo` if the project is empty, else warned; composite (atomic undo); path via `SFilePathRef` |
| `export-midi-file` | `clip`="" or `trackPath`="" or whole project, `filePath`, `type`="1" | not undoable; gate = `assert-file-identical` against a committed expected file for twSmf-authored input, `assert-midi-file` (counts) otherwise |
| `add-note` / `remove-note` | `clip`, `tick`, `dur`, `key`, `velocity`="100", `channel`="0", `releaseVelocity`="64", `take`="-1", `broadcast`="1" | note addressed by `(tick, key, channel)` |
| `set-notes` | `clip`, child `<n tick= dur= key= velocity= channel= …/>` — absolute new state of an addressed set, `take`, `broadcast` | THE batch verb: a piano-roll drag of N notes = one undo step; `mergeKey` = clip + selection hash |
| `add-event` / `remove-event` / `set-events` | `clip`, `kind`, `tick`, `channel`, `a`, `b`, `f`, `text`, `blob` | CC / bend / metadata; `set-events` batches CC drawing |
| `quantize-notes` | `clip`, `grid`="1/16", `strength`="1.0", `swing`="0", selection children | a `set-notes` composition; the inverse is the previous state |
| `set-midi-cut` | `clip`, `transpose`="0", `velocityScale`="1", `channel`="-1", `take`, `broadcast` | window fields go through the generalized `resize-clip` (frames in, converted once) — no second window verb |
| `set-tempo` | `bpm` | the ONLY tempo write; rescales `timebase=beats` links; inverse restores both |
| `set-link-timebase` | `clip`, `timebase`="beats\|time" | |
| `insert-plugin` | + `isInstrument`="false" in the descriptor | no new verb; instrument ⇒ slot 0, refused if one exists |
| `set-track-midi-output` | `trackPath`, `port`="" (portable device NAME), `channel`="-1", `offsetMs`="0" (signed, ±500) | per-machine ids resolve in `SSettings`, like the audio input device |
| `set-track-midi-routing` | `trackPath`, `routing`="auto\|parent\|none" | §3.2.1; absolute |
| `add-automation-lane` / `remove-automation-lane` | `owner` (trackPath \| clip \| trackPath+`slotIndex`), `target`, `mode`="read" | inverse carries the whole point list |
| `set-automation-mode` | `owner`, `target`, `mode`="off\|trim\|read\|touch\|latch\|write" | |
| `add-automation-point` / `move-automation-point` / `remove-automation-point` | `owner`, `target`, `time`, `value`, `curve`="linear", (`toTime`, `toValue`) | points addressed by old `(time, value)` |
| `set-automation-points` | `owner`, `target`, `from`, `to`, child `<p t= v= c=/>` | the batch/coalescing verb (curve drawing, Touch/Latch/Write commit); `mergeKey` = owner + target |
| `set-track-volume` / `set-track-mute` on a track with a Read lane | unchanged attributes | become a `set-automation-points` at the locator, else history and lane disagree |

Test-harness verbs (testkit): `assert-file-identical` (`actual`/`expected`, the
names PR #37's byte gate landed with; byte compare of two files — absolute paths
allowed, unlike `render`'s output name — for WAVs an optional `startFrame`/
`frameCount` range over the sample data) and `assert-log` (`contains`, `minCount`,
`maxCount` over the in-process `TwLog` ring — there is no log file under
`--test-case`; the ring's capacity is raised under `--test-case` and the count
is taken since the previous action, so a long render cannot evict the line
under test) land in **P0a** because every later phase leans on them;
`assert-midi-events` (count / kind / at / key / velocity / contains on a cut's
snapshot), `assert-midi-file` (counts / first tick), `assert-automation-value`
(`owner`, `target`, `time`, `value`, `tolerance`), `assert-midi-out` (against the
capture MIDI backend: `port`, `count`, `at`, `kind`, `key`, `channel`,
`tolerance`="4096" frames — frames measured through the audio capture backend's
clock, D6), `dump-midi-capture`, `assert-event-editor` (off-screen dock
`describe()` + `grabPng`), `virtual-key` (computer keyboard → note event at the
locator), `drag-note` and `drag-automation-point` (the `drag-clip-edge` twins),
`automation-write-tick` (one live tick of a Touch/Latch/Write pass, the
`slip-clip` shape), `assert-instrument-slot`. Every verb gets a row in
`action_roundtrip_test`. Undo is always asserted explicitly (`<undo count=…/>` +
a state assertion) — the runner's verify-undo pass compares track counts only (M6).

### 3.5 Serialization and versioning
New classes self-register by static initializer (OBJECT-lib rule); a new slice
`objects/midi` at the DAG rank of `objects/cut` (`check_layering.py` rows:
`objects/midi → {actions, model, persistence}` + engine `tw/events`, `tw/core`;
engine edge `events` is ALSO granted to `main/model` (`SProject` owns the
`twTempoMap`), `main/objects/track` (owns the `twEventClipSet`) and
`main/timeline` (the ruler reads the map)); `objects/track` must NOT depend on
`objects/midi` — the track consults MIDI-ness only through
`SObject::contentKind()`. `<SProject formatVersion='2'>` (D8a);
`kScannerVersion` 1 → 2 for the plugin cache (P2 adds descriptor fields).
Portable references: `.mid` import paths through `SFilePathRef`; MIDI device ids
are machine-local and live in `SSettings` keyed by a portable name.
`docs/ACTIONS.md` is hand-maintained (its "generated" header notwithstanding) —
each phase edits it by hand.

---

## 4. The engine

### 4.1 `tw/events` — a new leaf module (core only), no place in the dataflow DAG
- **One** definition of `twEventKind` / `twEvent` (review #2), shared by the
  engine snapshot, the event clip set, the plugin ABI (`tw/plugins → tw/events →
  tw/core`) and MIDI-out. Pinned so no phase has to invent it:
  ```
  enum class twEventKind : uint8_t {
    NoteOn, NoteOff, NoteChoke, NoteEnd, NoteExpression, PolyPressure,
    ControlChange, PitchBend, ChannelPressure, ProgramChange, Sysex, Midi1,
    ParamValue, ParamMod, ParamGestureBegin, ParamGestureEnd, Transport,
    // metadata (SMF meta + notation/tab/tracker) — sequence-only, never sent to a plugin
    Tempo, TimeSig, KeySig, Marker, Lyric, ChordSymbol, Articulation,
    StringFret, TrackerCell, Text, NoteAttr,
    Unknown = 0xF0 };
  struct twEvent {
    int64_t     time;      // frames (ClipPos) in a sequence / clip set; CHUNK-RELATIVE
                           // (0..n-1) in a process() call — same field, two documented uses
    twEventKind kind;  uint8_t flags;   // IsLive, DontRecord, Muted, SynthesisedOff
    int16_t     port, channel, key;     // -1 = wildcard / n/a
    int32_t     noteId;                 // host-issued, -1 = none
    uint32_t    paramId;                // ParamValue/Mod/Gesture/NoteExpression type
    double      value, value2;          // velocity/CC/bend/param/expr; tuning cents, bank, …
    int64_t     duration;               // NoteOn in a SEQUENCE only (notes are stored with length)
    uint32_t    payloadOffset, payloadSize;   // Sysex / text / Unknown blob → the OWNER's arena
  };
  ```
  A `twEventSeq` OWNS a byte arena its payload offsets index; the ABI's
  `twEventList` points into a host arena valid for the call (the processor copies
  the slice's payloads into its pre-sized arena when it rebases). Metadata kinds
  never reach `process()`.
- `twEventSeq` (immutable sorted vector + arena + `slice(a, b)` + `stateAt(P)`),
  `twTempoMap` (constant tempo now: `usPerQuarter`, `ppq`, `num/den`;
  `ticksToFrames(TickPos, srate) → Fraction`, `framesToTicks`; API shaped for
  segments; denominators `ppq·10⁶/gcd` are within `Fraction`'s red line — stated
  in the AC), `TickPos` in `tw/core/twdomains.h`, `twSmf` (SMF type 0/1
  reader/writer, PPQ rescale, meta events → kinds, unknown meta preserved),
  `twAutomationCurve` (proposal 12's second lane kind: sorted `{frame, value,
  curve, tension}`, `valueAt`, `fillRamp`), and **`twEventClipSet` + the
  `twEventSource` seam** (§4.2 — here, not in `tw/mix`, because `tw/plugins` may
  not include `tw/mix`, F15; edit methods return a core `twFrameRange`, not
  `twEditRange`).
- Unit-tested in `events_test`.

### 4.2 `twEventClipSet` (tw/events) — the event twin of `twTrackMix`'s clip list
Not a `twComponent` (like the processor is not): it produces no pages.
`insertClip/updateClip/removeClip/setClipMuted(key = opaque void*, startTime,
duration, resolver)` return `twFrameRange`; the resolver yields the module's OWN
record `twEventClipResolved { shared_ptr<const twEventSeq> seq; MapPosFn map; }`
(not `tw/graph`'s `twResolvedClip` — `tw/events` stays core-only); it implements **`twEventSource { collect(startPos, len, out) }`**
— (a) the **chase set** at `startPos` and (b) events in `[startPos, startPos+len)`
with page-relative offsets, clamped to each clip window, with the event twin of
the clip-end-bleed clamp: **note-offs are synthesised at the clip end for notes
still held, and note-ons before the window are only ever issued through the
chase**. Loop and slip go through the cut's map like audio (POSITION_DOMAINS
rules 1-3). Consumers: the instrument slot processor (§4.3) through the
`twEventSource` pointer, and the MIDI-out pump (§4.6) — both read the track's
**feed** (`twEventMerge` over the own set + bubbling children, §3.2.1), which is
also a `twEventSource`. Owned by `STrack`, one set per track (events are not per
bus).

### 4.3 The instrument slot (tw/plugins) — generator modes of the existing processor
Design option A (engine study): the instrument is **slot 0 of the existing
`twPluginChain`** with a `twPluginSlotProcessor` in a generator mode; the head tap
is otherwise unchanged. Everything from proposal 08 (descriptor, state chunk,
params, Missing placeholder with the *declared* layout, `setFactory`
re-resolution, the all-bus cache, `SPluginSlot` serialization) is reused verbatim.
Rejected: a `twInstrumentTrackMix` component (would grow its own processor/tap
split) and "instrument replaces the track's content component" (changes the graph
shape every invalidation walk assumes).

- **Sequenced after 35-B4 (§9.1):** by then a track has ONE N-channel page per
  component, one wide head tap, and no per-bus chains; the processor's "all-bus"
  cache is moot. Mapping rows (plugins inv. 16 amended): `0→C on a C-channel page`
  DirectGen (one instance); `0→1` MonoSpread; `0→2 on a 1-channel page` fold;
  `0→M, M>C` outs `0..C-1` to the page, `C..` into `Output.bus[M]` for **aux taps**
  (§5.4).
- **Pass-through sum** (D3): the head tap keeps its audio input plug (the bus's
  `twTrackMix` — the same mono clip page on every channel today) and adds it to
  the instrument output, so audio clips on an instrument track are heard; `x + 0.0f
  == x` makes "instrument present, no notes ⇒ byte-equal to the no-instrument
  render" the sharp gate. The chain rule that leaves a 0-input head unwired (F7)
  becomes "the head tap always has one input; the processor decides whether the
  plugin sees it". Keeps the graph shape (`bumpRenderChainEpoch*` walks unchanged).
- Events reach the processor per page through its `twEventSource*`:
  `collect(startPos, len)` → per-4096-chunk slice with rebased offsets →
  `process(in, outBuses, n, events, eventsOut, ctx)`; the processor also merges
  the UI `setParam` ring (offset 0) and the automation events (§4.5) into ONE
  sorted list per chunk (plugins inv. 5 amended).
- Continuity: D4 (`contiguous ⇒ process; else reset + chase + pre-roll K`).
  Instrument **bypass = silence but keep feeding events** (never the effect
  short-circuit, or un-bypass resurrects stale voices). `tailFrames()` sizes
  pre-roll and the project end. `reportedLatency()` stays reported-not-compensated
  (surfaced in the UI for instruments; PDC is a separate proposal).
- Instruments are **freeze-path only**: `calcOutputTo` on an instrument tap
  returns silence and logs once (the legacy pull is positionless and on proposal
  20's retirement list) — so `SMARAGD_REVAL_WORKERS=0` (legacy pull everywhere)
  makes instrument tracks silent by design; the testkit CONTRACT says so and
  instrument sweeps exclude 0. Preview freezes forward the upstream page (plugins
  inv. 6), so an instrument track's audio *waveform* preview is empty — the MIDI
  clip's thumbnail is app-side (§6.1); a level envelope can later be read from
  cached playback pages by position (34's pattern). Do NOT render plugins at 1 kHz.
- `twProcessContext {position, playing, tempoBpm, ppqPos, tsNum/tsDen,
  validFlags}` per call (F5): the tap's `renderPos_` + the tempo map. Reset the
  plugin's position at every reposition.

### 4.4 The run barrier — main thread, full path, instruments, render + play start (D4)
`SApplication::beginRun(pos)` walks the tracks whose slot 0 is an instrument and
calls `invalidateRenderPathRange(pos, INT64_MAX)` (the app-side path walk, F13)
and `slot->forgetContinuity()` (clears the processor's `lastEnd_/haveLastEnd_`
under its mutex), then returns. Called from `startRender` before the session
spawns and from play start immediately before `twSpeaker::startOutput()`. Never
from the readahead thread, never during playback seeks or loop wraps (F14). If a worker is mid-render at that position, verify-at-
publish self-staleness re-stales it (schedule inv. 8), so the wrong page is never
*current*.

### 4.5 `twGainStage` and automation consumption — D5
- `twGainStage` (tw/mix): one wide component per track (post-35) between the last
  tap and the rewire; scalar `gainDb` (the fader) × optional `twAutomationCurve`
  (Volume, dB-linear in fader space, `sfadercurve.h`) × mute (0 with a 1–2 ms
  ramp; NOT the parent's plug-nulling, which stays the *structural* mute for solo
  rules). Class ∞ pure ⇒ range invalidation is EXACT. Implements `calcOutputTo`
  (legacy pull) as `page × gain` so `assert-meter` and the meter tap (the rewire,
  post-fader post-FX either way) keep working. `twTrackMix::trackGainDb_` is
  forced to 0 dB in P3a and removed in P5.
- Processor: `setParamCurves(map<paramId, shared_ptr<const twAutomationCurve>>)`
  under `mutex_`; per chunk: value-at-chunk-start `ParamValue` (the parameter
  "chase" — pages render out of order), one point per breakpoint inside the chunk,
  a 64-frame dense ramp on continuous segments; `automationEpoch_` enters
  `stampNow_nolock()` (inv. 15). Every lane edit → `invalidateRenderPathRange(a,
  ∞)` from the `STrack` (pluginui inv. 6 still applies).
- Per-clip gain envelope (`cut:Gain`): applied to `childPage` before `mixFrom` in
  `twTrackMix::freezePage_nolock` — the clip window's `WarpedPos` domain through
  the same `clipToReaderMap`, so it trims/slips/loops with the clip.
  `cut:VelocityScale` / `cut:Transpose` are applied when the SMidiCut snapshot is
  built (event transforms, not audio).

### 4.6 MIDI devices and the out pump (tw/devices + app) — D6
`MidiOutput { open(id) / close / listPorts / createVirtualPort(name) /
send(bytes, n, hostTimeNs = 0) / latencyNs() }`, `MidiInput { … setCallback(bytes,
n, hostTimeNs) /* device thread, no Qt */ }`, `createMidiOutput()` selected by
`SMARAGD_MIDI_BACKEND = winmm | coremidi | alsaseq | capture | null | default`.
`MidiOutScheduler`: one std::thread, SPSC ring of `{dueHostTimeNs, port, 3–N
bytes}`, `WaitableTimer`/`nanosleep` at 1 ms granularity (`timeBeginPeriod(1)` on
Windows), joined Qt-free at shutdown. `MidiOutPump` (app, main-thread `QTimer`,
numbers in D6): reads the playhead atomic, walks tracks with `midiOutPort`,
`collect`s their event FEED over the lookahead window, converts channel remap,
applies the per-track offset, de-dups, enqueues; start/locate/loop-wrap/stop/panic behaviour per D6. The
capture MIDI backend records `{hostTimeNs, bytes}`; the AUDIO capture backend
records host time per delivered block; `assert-midi-out` maps through the latter.

---

## 5. The plugin ABI extension (format-agnostic core, three backends)

### 5.1 One header, one overload, no format types
`tw/plugins/twpluginevents.h` *uses* `tw/events/twevent.h`'s `twEvent`/
`twEventKind` exactly as pinned in §4.1 (`time` chunk-relative in a call; the
metadata kinds never appear) and adds `twEventList` (host-owned events + payload
arena, sized in `prepare()`), `twEventOut` (plugin → host sink, overflow counted
and dropped), `twProcessContext`. `twPlugin` gains `capabilities()` (`acceptsNotes, emitsNotes,
isInstrument, wantsMidi1Raw, supportsNoteIds, supportsNoteExpression,
emitsParamChanges, notePortsIn/Out`), `audioOutBusCount()/audioOutBus(i)`,
`tailFrames()`, and `process(in, outBuses[bus][chan], n, const twEventList&,
twEventOut&, const twProcessContext&)` whose default forwards to the legacy
`process()` — every existing backend, the null plugin and the tests compile
unchanged. `acceptsNotes()` stays as a forwarder for one release. Rules (new
plugins invariants): the host issues note ids and a NoteOff carries the same id or
−1; one sorted list per call; the same note is never sent in two dialects; the
VST3 event bus is activated at `prepare()` when present; instrument bypass keeps
feeding events.

### 5.2 Per-format mapping (the load-bearing gotchas)

| Ours | CLAP | VST3 | AU |
|---|---|---|---|
| NoteOn/Off/Choke | `clap_event_note` on a declared note port; dialect negotiated per port (`clap_host_note_ports.supported_dialects` = CLAP\|MIDI); note_id per on | `kNoteOnEvent/kNoteOffEvent` with matching `noteId`; the kEvent input bus MUST be `activateBus`'d — today only audio buses are | `MusicDeviceMIDIEvent(0x9n/0x8n, inOffsetSampleFrame)` posted BEFORE `AudioUnitRender`; MusicDevice has no input element (skip the render-callback path) |
| CC / bend / pressure / PC | raw `clap_event_midi` (dialect MIDI) or note expression | **no event type**: `IMidiMapping::getMidiControllerAssignment(bus, ch, cc)` per CC once at prepare → a parameter change point at `sampleOffset`; `kPitchBend=129`, `kAfterTouch=128`, `kCtrlProgramChange=130`; unmapped CCs dropped | `MusicDeviceMIDIEvent` 0xBn/0xEn/0xDn/0xCn |
| NoteExpression | `clap_event_note_expression` (volume/pan/tuning/vibrato/expression/brightness/pressure) | `kNoteExpressionValueEvent`, types from `INoteExpressionController` on the CONTROLLER, requires noteId | MIDI 2.0 per-note via `MusicDeviceMIDIEventList` (later) |
| ParamValue (automation) | `clap_event_param_value.header.time = offset` (today always 0) | `IParamValueQueue::addPoint(offset, v)` — the queue supports it, the caller passes 0 today; ONE queue per param sorted, fed by the UI ring AND the automation slice | `AudioUnitScheduleParameters` (`Immediate` + `startBufferOffset`, or `Ramped`) |
| Notes OUT | `out_events` (host `try_push`; today discards) | `ProcessData::outputEvents` — needs an activated kEvent OUTPUT bus + host `IEventList` | `kAudioUnitProperty_MIDIOutputCallback`, set before `AudioUnitInitialize` |
| Transport | `clap_process::transport` + a transport event at time 0 | `ProcessContext {kPlaying, kTempoValid, projectTimeSamples, tempo, projectTimeMusic}` (today `nullptr` — arps cannot sync) | `kAudioUnitProperty_HostCallbacks` (pull) |
| Multi-out | non-main audio ports (collected today, discarded) | aux buses (`BusInfo.busType == kAux`; today `activateBus(false)`) | output elements 1..N, one `AudioUnitRender` each at the same timestamp |
| Enumeration | `CLAP_PLUGIN_FEATURE_INSTRUMENT` (done) | `subCategories` contains "Instrument" (done) | add `aumu` (MusicDevice) and `aumi` (MIDI processor) to `twaumodule.cc` |

VST3 host support list grows: `IEventList`, `IMidiMapping` query, `INoteExpressionController`, `IPlugInterfaceSupport` entries.
CLAP host extensions grow: `clap_host_note_ports`, `clap_host_params` (33 M4), `clap_host_tail`.

### 5.3 Test instruments (D7)
- `tw.test.clap.sine`: features INSTRUMENT|SYNTHESIZER; 0 in, 1 stereo main out
  [+ optional aux mono out for the multi-out gate]; note port dialects CLAP|MIDI,
  preferred CLAP; 16 sine voices, oldest-steals, `amp = velocity`, no envelope,
  instant on/off (RMS = vel/√2 · gain, frequency exactly the key), NOTE_END pushed
  on off (event-out matching), PARAM_VALUE id 0 = gain applied AT `ev.time` (the
  automation-offset gate), `CLAP_PROCESS_ERROR` on over-size blocks or a wildcard
  note-on. `tw.test.clap.arp`: note in/out, holds pressed keys, emits on a fixed
  frame grid via `try_push` (event-out plumbing; MIDI-out later).
- `twtestvst3.cpp` `TestSine`: SPLIT component/controller, `Instrument|Synth`, 0
  in / stereo out, one kEvent in bus, reads `inputEvents`, honours `noteId`,
  **ignores an unactivated event bus** (a host that forgets `activateBus(kEvent…)`
  fails the level assert), Gain via `inputParameterChanges` honouring
  `sampleOffset`, `IMidiMapping` CC7 → Gain.
- `twNativeInstrument` (tw/plugins builtin, `format="tw"`, uid `tw.native.303`):
  monophonic; params {cutoff, resonance, envMod, decay, accent, waveform, slide};
  velocity ≥ 100 = accent; overlapping notes = slide; deterministic (dsp inv. 2);
  present in every build, every worktree; visible in the browser like the built-in
  `twPassThrough` is today.

### 5.4 Multi-output (designed here, built in P9)
`Output.bus[M]` on the processor + a tap `(processor, out ≥ C)` placed as the
*content component* of a **return track** IS the multi-out design with zero new
engine types; missing are the app model (a return track object referencing a
`(track, slot, out)` triple, serialized like `pluginChainId`), cache sizing, and
UI. Named here so P3 does not paint over it.

### 5.5 Scan / probe
Descriptor gains `acceptsNotes, emitsNotes, nOutBuses (+ per-bus channels),
eventPortsIn/Out`; `kScannerVersion` 1 → 2 (invalidates the cache once, sticky
failures included); probe JSON schema bumped; the probe already instantiates each
plugin, so no new cost and no `activate()` in the probe. Browser: Kind column +
filter; "Add Instrument" opens it filtered.

### 5.6 The tw303a synth
The unit generators are graph components with audio-rate control inputs; the app
never uses them, and there is no note/voice model — proposal 12's "first consumer"
does not exist as a consumer. `twNativeInstrument` lifts the arithmetic (saw,
ladder, portamento) into buffer-level functions inside a `twPlugin`; the old
components stay for the tests that use them.

---

## 6. UI

### 6.1 Arranger (timeline)
- Event clip thumbnail: `SMidiSequenceRendererInline` (note rects scaled to the
  present pitch range, CC as faint lines, metadata as tiny top-edge glyphs like the
  onset strip) via the existing polymorphic `getInlineRenderer()` path (U1);
  `SCutRendererInline`'s `container` heuristic is replaced by `contentKind()`.
  Loop tiling and selection frames come free from `STrackRendererInline`.
- Insert: extension-dispatch on `registerExternFileFactory` (today a single
  factory) + `.mid` in the insert filter + OS `text/uri-list` drops.
- Track head (`SSMVMixerControl`, ~13 px spare at 120 px; Full/Compact/Tiny): a
  second button column shown in Full/wide only, menu otherwise — automation mode
  "A" (cycles Off/Trim/Read/Touch/Latch/Write, right-click menu), lane fold,
  instrument "I" (opens the param editor; native editor per proposal 33 M3),
  MIDI-out selector, MIDI-in LED (pumped from `meterTick`, never a MIDI thread).
  `describeTrackHead()` (a `describeTrackMeter` sibling) is the test seam. Track
  colour + kind glyph derived (D3).
- Automation lanes: `STrackRow` sub-lane kind `{Take, Automation}` + target;
  `drawAutomationLane` in a NEW helper file (not another 300 lines in
  `sstdmixerview.cpp`); per-track shown-lanes set beside `takesExpanded_` (prune on
  delete — proposal 30 §E.5, done once for all sets); context-menu picker "Show
  automation ▸ Volume / Mute / <plugin params>"; gestures: click = add point,
  drag = live + one `move-automation-point`, Alt-drag = tension, marquee, Delete;
  clip envelope = overlay in the cut renderer after `drawWarpMarkers`.
- FX strip: instrument-first row (icon, no bypass toggle), "+ Add Instrument"
  (disabled once present), `reorder-plugin` refused across slot 0,
  `describeSlot()` gains `kind=`.
- Clip Properties dock: an `SMidiCut` page (transpose, velocity scale, channel,
  timebase) lands with P1 — today the panel assumes `SCut`.

### 6.2 The event editor
A **selection-following dock** (fifth `QDockWidget`, bottom, tabified with Log) —
the exact `SClipPropertiesPanel` shape (timeline inv. 8/9: re-resolve selection
paths on `arrangementChanged`, no cached `SLink*`, commit on release). Proposal
09's tab shell does not exist; the same widget is later hostable in a tab via
`sdetaileditors::registerEditor`. New module `main/eventui/`: `SEventEditorDock`
→ toolbar (select/draw/erase, grid, quantize, kind switch) + `SEventTimeRuler` +
abstract `SEventEditorView` (`setClip(path)`, `setTimeAxis`, snapshot + actions)
with a static-initializer **kind registry**: `SPianoRollView` first (+ velocity /
CC lane stack), `SStepGridView` (tracker) and score/tab later — all bound to the
same clip snapshot and the same batch verbs. `SEventTimeAxis` links to the
arranger's `secondWidth_/upperLeftX_` (one px↔frame conversion, timeline inv. 4).
Grid: reuse `STimeGridSpec`, add divisions to `SSnapSpec`, consume the tempo map
(the ruler's 480-PPQ display becomes the map's 960).

### 6.3 Options, transport, keyboard
Options → MIDI page mirroring the Audio page (`buildMidiPage/load/apply` over
`MidiOutput::listPorts`; inputs multi-select, outputs, "Create virtual port"
gated per platform like MP3, latency offset, "computer keyboard as MIDI input");
new `SSettings`/`SOpt` keys; no device-thread callbacks into Qt. Transport: reuse
arm "R" and its channel menu for MIDI arm; `set-tempo` action + bars.beats
readout; `SVirtualKeyboardDock` (two painted octaves, REAPER key map, must not
steal Space) doubles as the headless note source (`virtual-key` verb) and, in
P8, as a live input device. Metronome stays a stub (engine work, not here).

### 6.4 Testability
House pattern: build the real widget off screen + `describe()` string +
`contains=`; PNG grabs are coverage, not oracles. New verbs in §3.4. Known lack:
a generic `widget-gesture` (press/move/release/keys on a named widget) — P4 adds
`drag-note`/`virtual-key`, P6 `drag-automation-point`; a generic seam is P9.

---

## 7. Threading and invariants (new or amended; each phase's brief names its subset)

Threading, unchanged in kind: event tables, cut snapshots and curve snapshots are
immutable and swapped under the owner's mutex (rule 2); the scheduler holds a
snapshot for the duration of a freeze; the MIDI scheduler and the capture MIDI
backend are Qt-free std::threads publishing through rings/atomics (rule 1); the
MIDI-out pump and every UI indicator run on the main thread off `meterTick`/a
`QTimer`; the run barrier is issued from the MAIN thread through the SObject walk
before demanding (F13, F10); every fix stays order-independent (rule 4). New
thread in the inventory table: `MidiOutScheduler`.

| Contract | Amendment |
|---|---|
| CLIP_MODEL | fourth kind of child: same placement/window layers, engine view = event clip set entry, not `ClipEntry`; note-offs synthesised at the clip end; split is non-destructive (window gating); take stacks over `SClipWindow`, homogeneous by content kind; `SLink::timebase` |
| POSITION_DOMAINS | new domain row **Ticks** (musical; `SMidiSequence` events and the `SMidiCut` window; only `twTempoMap` converts) and rule 7: "an `SMidiCut` speaks frames on every side the track sees; the tick→frame conversion happens exactly once per value, inside the cut" |
| FREEZE_PROTOCOL | class-1 run protocol paragraph: reposition = reset + chase + pre-roll; the render barrier is a main-thread full-path invalidation; playback splices are accepted; state blobs per page are NOT captured |
| plugins/CONTRACT | inv. 5 (chunk-relative events), 6 (instrument preview = silence), 16 (generator rows), the discontinuity paragraph → "reset + pre-roll + replay"; new: note ids, one sorted list / one dialect, VST3 event bus activation, instrument bypass keeps events, `automationEpoch_` in the stamp, instruments freeze-only |
| mix/CONTRACT | `twGainStage` between chain and rewire is THE fader (freeze + legacy pull); `twTrackMix` gain retired; per-clip gain envelope applied pre-mix |
| events/CONTRACT (new) | one `twEvent`; `twEventClipSet` mirrors the ClipEntry rules; `twTempoMap` is the only tick↔frame converter; the tempo authority |
| schedule/CONTRACT | class-1 edits invalidate `[a, ∞)`; the barrier is not a scheduler feature (main-thread walk) |
| model/CONTRACT | `contentKind()`; inline `<events>`/`<automation>` are the sanctioned non-SLink payloads; the loader ignores them for ordering; prune-and-retry per element kind |
| persistence/CONTRACT | `formatVersion` (warn, never refuse); missing-target policy per element kind |
| track/CONTRACT | event clip set ownership + sync (same key rule as clips); automation lanes are owner-held, never `SLink` children; instrument slot rules; Volume/Mute reach the post-FX gain stage; project end includes the instrument tail |
| cut/CONTRACT | `SClipWindow` + wrap factory; "windowed verbs never `dynamic_cast<SCut*>` for arithmetic the interface provides" |
| timeline/CONTRACT | inv. 2 names `contentKind()`; inv. 5 wording includes automation lanes; the event editor is a selection follower (inv. 8/9) |
| testkit/CONTRACT | `SMARAGD_MIDI_BACKEND=capture` default under `--test-case`; the test instruments; `SMARAGD_REVAL_WORKERS=0` ⇒ instruments silent; undo is asserted explicitly; the `assert-meter` legacy-pull caveat is retired for gain and extends to event/automation edits |
| THREADING | inventory row `MidiOutScheduler`; MIDI-out at play time; the barrier is main-thread only |
| docs/ACTIONS.md, CLAUDE.md | every verb in §3.4; the "legacy pull does not see gain" paragraph in "Level meters" is rewritten in P3a |

---

## 8. Phases and gates (summary — the briefs are in `36_ORCHESTRATION.md`)

Each phase is one Opus 5 sub-agent's unit of work with an entry condition and a
gate written as acceptance criteria. Independent phases run in parallel worktrees.
Standing gate for every phase: `./build.sh` (re-configure), `check_layering.py`,
`check_logging.py`, `ctest` with the registered/run/skipped count reconciled,
`repeat_test.sh` N ≥ 50 × workers {1,4,8,16} for anything touching the scheduler
or a class-1 processor, and **byte-identical renders of the existing golden
corpus** unless an AC licenses a re-freeze with a written justification.

| Phase | Deliverable | Depends on | Gate (headline ACs) |
|---|---|---|---|
| **P0a** Persistence tolerance + `SClipWindow` + testkit basics | prune-and-retry loader; `formatVersion`; `SClipWindow` + wrap factory; every cast site dispatches on it; `contentKind()`; `assert-file-identical`, `assert-log` | — | `load_unknown_object_survives` + `load_missing_sample_placed_survives`; every windowed/take verb round-trips; goldens byte-identical (incl. `plugin_*`) |
| **P0b** `tw/events` leaf | `twEvent` (the one), `twEventSeq`, `twTempoMap`, `TickPos`, `twSmf`, `twAutomationCurve`, `twEventClipSet` + `twEventSource` + `twEventMerge`, `events_test` | — | event-table-equal SMF round-trip (byte-identical for twSmf-authored files); `stateAt` vs brute force; tick↔frame exact at 3 rates; `collect` clamps + synthesised note-offs; layering leaf |
| **P1** Event clips in the model | `SMidiSequence`, `SMidiCut` (tick window), `SLink::timebase`, `objects/midi`, verbs, `STrack` → event clip set + folder feed (`twEventMerge`, mute/solo, `midiRouting`), thumbnail, Clip Properties page, `assert-midi-events/-file`, `set-tempo` as the only tempo write | P0a, P0b | import → save → load → export equal; non-destructive split; stretch/loop/slip/take verbs; `set-tempo` moves beat-timebase MIDI links and re-maps notes, moves no audio; explicit undo asserts; MIDI-only project renders silent without a `twview` warning (`assert-log`) |
| **P2** Plugin ABI events + fixtures + native 303 (`twPlugin` level only) | `twpluginevents.h`, `process()` overload, capabilities, tail, aux-out discovery; CLAP/VST3/AU translation; `tw.test.clap.sine/arp` + a `clipThreshold` param on `tw.test.clap.gain`, VST3 `TestSine`, `twNativeInstrument`; scanner v2 | P0b (`twevent.h`) | `plugins_test` at the `twPlugin` level: notes → sine RMS + frequency per format; mid-block gain step at the offset; unactivated-bus teeth; arp event-out; effect goldens byte-identical; **no processor / chain change** |
| **P3a** Fader post-FX | `twGainStage` (freeze + legacy pull), fader/mute wiring, trackmix gain forced to 0 dB, docs | 35-B4, P2 (clipper) | all goldens byte-identical (by construction, P4); new `fader_post_fx.qxa` value case + ORDER case (clipper) that fails on the pre-move binary; `meter_*` green |
| **P3b** Instrument slot + event feed | generator modes (post-35 wide), pass-through sum, `twEventSource` feed (the track FEED incl. bubbling children), reset+chase+pre-roll, slot rules, browser/strip/head minimum, project end + tail | P1, P2, P3a, 35-B4 | `instrument_sine_render` (freq/RMS per note), `instrument_mixed_track` (no-notes ⇒ byte-equal), `instrument_folder_drums` (parent instrument plays two children; child mute drops its part), edit reaches the render, native 303 presence; `repeat_test` sweep |
| **P3c** Render barrier + determinism | `beginRun` main-thread walk (path invalidation + `forgetContinuity()`) at render start and play start | P3b | `instrument_render_determinism` (`cmp` two in-process renders around a playback + one fresh process); `instrument_locate_continuity` (303 warmth + sine chase after `set-locator`); playback seek splices stated as NOT gated |
| **P4** Event editor (piano roll) + virtual keyboard | `main/eventui`, dock, view base + kind registry, piano roll + velocity/CC lanes, time axis link, grid/quantize, `virtual-key`, `drag-note`, `assert-event-editor`, `describeTrackHead` | P1 (P3b for audible checks) | explicit undo per editor op; off-screen `describe()`; PNGs; `virtual-key` → render → `assert-audio-frequency` |
| **P5** Automation model + engine | `SAutomationLane`, `ParamRef`, verbs, curve snapshots, gain-stage ramps, processor param events + `automationEpoch_`, clip gain envelope, range invalidation | P0b, P3b | `automation_volume_ramp` (per-second RMS; render `cmp`), `automation_mute_step` (ramp windows between levels), `automation_plugin_param` (step at a mid-page-1 offset, CLAP + VST3), `automation_clip_gain`, `assert-automation-value`; goldens without lanes byte-identical |
| **P6** Automation UI | sub-lanes, painting, gestures, picker, head mode button, Touch/Latch/Write recorder committing one action, plugin-gesture punch-in, clip envelope overlay, fader shows the read value | P5 | `drag-automation-point` + `automation-write-tick` cases (`<undo count="1"/>`); `assert-lane-alignment` over automation lanes; head `describe()` at three densities; `sstdmixerview.cpp` growth ≤ 100 lines |
| **P7** MIDI output | `tw/devices` MIDI interfaces + WinMM/CoreMIDI/ALSA-seq/capture/null; scheduler thread; out pump over the track FEED; per-track offset; audio-capture host-time log; `set-track-midi-output`; Options MIDI page; chase; panic | P1 (P3 optional) | `midi_out_capture` (notes at expected frames ± 4096 through the audio clock; chase after locate; all-notes-off on stop; loop wrap; `offsetMs=200` shifts every event 9600 frames earlier; a folder parent's port carries its children's notes); goldens unaffected; WinMM jitter NOT gated |
| **P8** Live MIDI input + recording *(outline; gated on proposal 21 P1)* | `MidiInput` backends, event ring → processor merge point, computer keyboard as input, record to clip with takes | 21-P1, P3b, P7 | to be written when 21-P1 lands |
| **P9** Follow-ups *(outline)* | multi-out → return tracks; tempo segments (37); MIDI-FX routing (arp → instrument in-app); score/tab/tracker views; `split-notes-at`; generic `widget-gesture`; PDC | P3/P5 | own proposals |

Parallelism: P0a ∥ P0b at the start; P1 (after P0a+P0b) ∥ P2 (after P0b); P3a
after 35-B4 + P2; P3b after P1+P2+P3a; P3c after P3b; P4 ∥ P5 after P3b; P6 after
P5; P7 after P1. Critical path: P0 → P1/P2 → (35-B4) → P3a → P3b → P3c → P5 → P6.

---

## 9. Dependencies on other proposals

### 9.1 Proposal 35 — configurable multichannel signal flow (on `feat/multichannel`) — HARD sequencing
35-B1 is "the big sweep" over every `freezePage` override, the latch seam, capture
and metering; 35-B4 retires per-bus taps, per-bus chains and the processor's
all-bus cache. **P2 therefore touches no processor, chain or tap code** (ABI,
backends, fixtures, scanner, the native instrument only), and **P3a/P3b start
only after 35-B4 has merged** — the gain stage and the generator head tap are
written once, as wide components. If 35 stalls, the orchestrator may re-cut P3
against the per-bus world explicitly (both variants are described in §4.3/§4.5),
and 35 rebases; that is a decision recorded in STATE.md, never a drift. Until
35-B5 lands the sink is mono, so P3's audio assertions read channel 0 only and
**never assert `L != R`**. Pan automation (`self:Pan`) is unlocked by 35-B5 and is
explicitly out of P5.

### 9.2 Proposal 21 — real-time dataflow (live lane)
`process()` takes an event list in both modes; the processor is the merge point
(clip-set slice vs SPSC ring from a `MidiInput`). Nothing else is designed here;
P8 is an outline that 21-P1's LiveGraphPump makes concrete.

### 9.3 Proposal 32 — project versioning
Placement-scope envelopes and slot-index-free automation addressing wait for
stable UUIDs; until then lanes are stored *on* their owner (D5) and
`formatVersion` is added here (D8a).

### 9.4 Proposals 20 / 33
20's legacy-pull retirement removes the "instruments are freeze-only" caveat and
its `assert-meter` blind spot; 33 M3 turns the head's "I" button from the generic
param editor into the native editor.

---

## 10. Risks

| Risk | Mitigation |
|---|---|
| The fader move (D5) changes float ordering for a track with a non-linear insert at a non-unity fader | No such golden exists (P4); the new fixture asserts a closed form; future goldens are frozen post-move |
| Chase alone restarts envelopes on pads (audible after a locate) | Pre-roll K subsumes it for notes started inside the window; K per slot; the locate case is a playback property, gated by warmth (303) + presence (sine) on the capture backend, not `cmp` |
| The render barrier costs a re-render of the readahead window when play starts on instrument tracks | Only tracks with an instrument; only at render/play start; measured in P3c |
| Playback seek splices are not barriered | Same as effects today; chase + pre-roll bound the damage; stated as NOT gated in every PR |
| Ticks-in-window (D2) couples `SMidiCut` duration/anchors to the tempo | One class listens to `bpmTempoChanged`; LIFO undo keeps frame-captured inverses exact; take stacks homogeneous |
| `set-tempo` moving beat-timebase links surprises audio-first users | REAPER's default; `set-link-timebase` per clip; audio never moves |
| VST3 CC → `IMidiMapping` per CC per plugin is per-format special casing | Confined to the backend; the split-controller `TestSine` gates it |
| A MIDI project does not open in older builds | D8a lands FIRST as its own PR; documented in the persistence CONTRACT |
| `sstdmixerview.cpp` grows again | Automation lane painting and gestures go into new files; the event editor is a new module; a line-count AC |
| WinMM has no timestamps and no virtual ports | Send-at-due-time in the scheduler thread; virtual ports offered only where the OS supports them; jitter reported as not gated |
| Legacy pull path (`assert-meter`) does not see event/automation edits | Documented; disappears with proposal 20; the gain caveat is retired by the gain stage |
| Proposal 35 stalls | §9.1's explicit re-cut option |

## 11. Decisions taken in v2 (requester accepted all six on 2026-08-15; v2.2 adds 7–8 from the requester's own challenges)

1. **Pass-through sum** on an instrument track (audio clips audible; REAPER
   overwrites, Cubase/Logic disallow) — chosen for a kind-less track; centre-panned
   mono into a stereo instrument until 35 gives clips channels.
2. **Note-on chase on locate**: instruments ON (pre-roll makes it inaudible),
   MIDI-out OFF by default (setting).
3. **Trim/Read** as the default automation mode (REAPER); the fader value stays
   meaningful when a lane exists.
4. **Split is non-destructive** (window gating, REAPER default); `split-notes-at`
   later. Content is shared, so a split must not edit it.
5. **MIDI placements follow the beat by default** (`timebase=beats`), audio stays
   time-locked (REAPER defaults).
6. **One instrument per track** (single event feed) — REAPER allows several VSTi;
   revisit with MIDI-FX routing (P9).
7. **Events bubble up folder hierarchies** like audio: a child with no local
   consumer (no instrument, no MIDI-out) feeds its parent's instrument / port;
   `midiRouting = auto | parent | none` overrides; mute/solo respected; note ids
   namespaced per child (§3.2.1).
8. **Per-track adjustable MIDI-out delay** (`midiOutOffsetMs`, ±500 ms, positive
   = earlier) for outboard round-trip latency, plus a global default (D6).

## 12. Deliberately not done here
Tempo segments/ramps (37); return tracks / multi-out UI (P9); MIDI-FX chains
between slots (P9); score/tab/tracker views (P9); MPE editing; MIDI clock/MTC;
external-instrument audio return; PDC; placement-scope envelopes (32); pan
automation (35-B5); live input/monitoring/recording (21 + P8); metronome engine;
a playback-run barrier (would need an RT page-boundary swap policy).

## 13. Adversarial review (v1 → v2)

| # | Sev | Finding (short) | Resolution in v2 |
|---|---|---|---|
| 1 | BLOCKER | The barrier invalidated taps only; nothing above them re-renders (F13) | D4/§4.4: main-thread full-path `invalidateRenderPathRange(pos, ∞)`; render start + play start/locate; instruments only |
| 2 | BLOCKER | Two `twEvent` types in two parallel phases | §4.1/§5.1: ONE `twEvent` in `tw/events`; `tw/plugins → tw/events`; P2 depends on P0b |
| 3 | BLOCKER | plugins ↔ mix may not include each other; `twEditRange` in mix | `twEventClipSet` + `twEventSource` live in `tw/events`; `twFrameRange` in core |
| 4 | BLOCKER | MIDI cut window in frames contradicts every tempo consequence; take stacks inherit it | D2: tick-native window; `SClipWindow` in own units + frame-facing reads; LIFO undo argument; homogeneous stacks |
| 5 | BLOCKER | Log-file ACs; the loader already warns twice | `assert-log` over the in-process ring (P0a); "≥ 1", never "exactly one" |
| 6 | MAJOR | A playback barrier adds a mid-page RT switch on every seek (F14) | Render + play-start only; playback splices accepted and NOT gated |
| 7 | MAJOR | Fader-move byte-identity claimed on fixtures that don't combine fader + plugin; float non-associativity; legacy pull | P4; new closed-form fixture; gain stage implements `calcOutputTo`; docs caveat retired |
| 8 | MAJOR | P2/P3 rewrite what 35-B1/B4 rewrite | §9.1: P2 = ABI/backends/fixtures/scanner only; P3a/b after 35-B4; explicit re-cut option |
| 9 | MAJOR | verify-undo compares track counts only | Every AC asserts undo explicitly |
| 10 | MAJOR | "Split cuts a straddling note" edits shared content; rounding | Non-destructive split by window gating; `split-notes-at` later |
| 11 | MAJOR | Byte-identical SMF round-trip of a foreign corpus impossible with duration-paired notes | Event-table equality; byte-identity for twSmf-authored files |
| 12 | MAJOR | `assert-midi-out` mapping circular; no pump numbers | Independent audio-clock mapping; 20 ms / 250 ms / de-dup / loop-wrap rules |
| 13 | MAJOR | Time-locked MIDI position diverges from every DAW default | `SLink::timebase`, MIDI default `beats`; `set-tempo` rescales |
| 14 | MAJOR | Pre-roll byte-exactness only under unstated constraints | Sine fixtures for bytes; 303 for warmth/presence; constraints written into the AC |
| 15 | MAJOR | P3 AC3 tested neither pre-roll nor chase | P3c: 303 warmth + sine chase; `set-locator` (no `seek` verb) |
| 16 | MAJOR | Loader sweep policy wider than admitted; placed-missing-sample already fatal | D8a: prune-and-retry per element kind; second fixture |
| 17 | MAJOR | `SClipWindow` under-scoped (className compare, take helpers, unsplit, select/remove-take; no wrap factory) | D8b: full cast list + wrap factory |
| 18 | MAJOR | Wrong module lists (SPluginChain/Slot are in objects/track; tempo writes touch timeline+shell) | Corrected in §3.3 and every brief |
| 19 | MAJOR | Verbs relied on but not delivered (byte cmp, log, drag-note, seek) | `assert-file-identical`, `assert-log` (P0a); `drag-note` (P4); `set-locator` |
| 20 | MAJOR | P5 arithmetic (70000 is page 1) and ill-posed "no click" | Fixed; ramp-window RMS between levels |
| 21 | MAJOR | Missing: tail vs project end, sample-rate rebuild, `REVAL_WORKERS=0`, MIDI-out loop wrap, SMF tempo maps, containers, `TickPos` type, `kCacheEntries` | All addressed in §3.1/§4.3/§4.6/D6; `kCacheEntries` claim dropped |
| 22–34 | MINOR | tempo authority; Fraction red line; unknown attributes; mute lane vs button; stale docs; ACTIONS.md hand-maintained; `plugin_*` in P0a goldens; cross-binary check is orchestrator-run; single instrument reason; centre-mono sum; `<undo count>`; Clip Properties for SMidiCut; §11 timing | All folded in (D2, §3.1, §3.4, §3.5, §6.1, §11, briefs) |

### 13.2 Verification pass (v2 → v2.1)

The reviewer re-read v2: #2–#6, #8–#14, #16, #17, #19–#21 RESOLVED; four
PARTIALLY, plus eleven items the rewrite introduced. All applied:

| Item | Change in v2.1 |
|---|---|
| #1 partial — an epoch bump does not clear `lastEnd_` | `forgetContinuity()` on the processor, called by `beginRun` (D4, §4.4, P3b/P3c) |
| #7 partial — a linear insert cannot gate pre/post order | `tw.test.clap.gain` gains a hard-clip threshold (P2); `fader_post_fx` gets an order-sensitive case (P3a AC2) |
| #15 partial / new (a) — K = one chunk cannot warm a 303 held since 0 s | K reaches back to the earliest held note's start (≤ 4 s), D4 |
| #18 partial — engine `events` edge for model/track/timeline | §3.5, P1 AC6 |
| (b) `twEvent` under-specified (uint32 vs ClipPos, meta kinds, payload ownership) | pinned struct + arena, §4.1; §5.1 refers to it |
| (c) P3b AC3 fails if a note sounds before the added one | fixture constrained; compare `[0, 96000)` with nothing sounding before 2 s |
| (d) wrong `twview` log text | exact text in P1 AC4 |
| (e) `twResolvedClip` is tw/graph | `twEventClipResolved` in tw/events, §4.2 |
| (f) frames-only rescale drifts | `beats` links carry exact `startTicks`; frames derived; nested walk (D2) |
| (g) `beginRun` sites | before `twSpeaker::startOutput()`; the locate-while-stopped site dropped |
| (h) grep pattern | `"SCut"` literal (P0a AC5) |
| (i) capture host-time log RT-safe; `assert-file-identical` absolute paths | §3.4, P7 |
| (j) dependency graph vs table | graph redrawn (orchestration §3.0) |
| (k) P3a "legacy pull now sees gain" | AC3 licenses the agent to report, not fix elsewhere |
| `assert-log` ring capacity | raised under `--test-case`; counted since the previous action |
| P0a AC7 | moved to P1 definitively |

## 14. Glossary (ours → the references)

| Smaragd | REAPER | Logic | Cubase | Studio One |
|---|---|---|---|---|
| event clip (`SMidiCut` over `SMidiSequence`) | MIDI item / take source | MIDI region | MIDI part | instrument part |
| shared content (one sequence, many cuts) | pooled MIDI item | alias | shared copy | shared copy |
| clip modifiers (transpose, velocity scale, channel) | take pitch / item props | region parameters | MIDI modifiers | part inspector |
| link timebase (`beats` / `time`) | item timebase | (global musical) | track Musical/Linear | track Beats/Seconds |
| instrument (slot 0 with a role) | VSTi in the FX chain | software instrument | VST instrument | instrument |
| track MIDI output (`midiOutPort/Channel`) | MIDI hardware output | External Instrument | MIDI track output | External Instrument |
| automation lane (track, timeline time) | track envelope | track automation | track automation | track automation |
| clip envelope (window, clip time) | take envelope | region automation | part controller lane | part automation |
| Trim/Read · Read · Touch · Latch · Write | Trim/Read, Read, Touch, Latch, Write | Off, Read, Touch, Latch, Write, Trim | Read/Write + Touch/Auto-Latch/Cross-Over | Off, Read, Touch, Latch, Write |
| tempo map (constant now) | tempo envelope | tempo track | tempo track | tempo track |
| chase | "Chase MIDI note-ons" | chase settings | Chase Events | chase |
