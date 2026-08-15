# Proposal 36 — Event clips, instruments, MIDI output and automation

> **Status: DRAFT v1 (2026-08-15).** Design + phased plan; no code. Written in a
> dedicated worktree (`docs/midi-instruments-automation`) from five parallel
> concept studies (engine, app model, plugin ABI + OS MIDI, UI, industry survey of
> REAPER / Logic / Cubase / Studio One / Live / Bitwig / Ardour) and one
> adversarial review (§13). Execution companion: `36_ORCHESTRATION.md` — one
> Opus 5 sub-agent per phase, every gate written as acceptance criteria.
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
> `35_MULTICHANNEL_SIGNAL_FLOW.md` (on `feat/multichannel`; §9.1 below).

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
built in P9), score / tab / tracker *editors* (the data model carries their events
from P1; the views are P9), MIDI clock / MTC, MPE editing UI, external-instrument
audio return, plugin-delay compensation.

---

## 1. What is true today (the facts the design is built on)

Every claim was verified in the worktree; the file:line anchors live in the five
study reports and are repeated here only where they decide something.

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

**App model**
- M1. Q_PROPERTY inventory, whole app: `SObject{Solo, Muted, ArmedForRecording,
  Volume, Pan, Delay, SName}`, `SCut{Stretch, PitchCents}`. **Pan and Delay are
  dead** (setters emit, nobody listens). Only Volume and Muted reach the engine.
- M2. `SLink` = `{SObject&, startTime}`; serialized with no children and no id.
  Object ids in files are pointer values; there is **no format version attribute**
  on `<SProject>` (proposal 32 M0 is DRAFT). Tempo is one scalar `bpmTempo_`.
- M3. Only `move-clip`/`remove-clip` are placement-generic; `split/resize/duplicate/
  set-clip-name/take-*` are hard-coded to `SCut`; `STakeStack` is a stack of
  `SCut`s. `SCut` cannot window non-audio content (its non-random-source branch is
  the *container* capture path).
- M4. Loader: flat top-level objects ordered by `<SLink objectId>` only; an unknown
  element name yields NULL and is skipped; every link that referenced it is
  unresolvable; **the leftover sweep then drops the containing track, then the
  mixer, then fails to find the root** — a project with one unknown object type
  does not open at all in an older build.
- M5. `STrack::trackChildWasAdded` inserts every duration-carrying child into every
  bus mixer keyed by `SLink*`; a null resolved component logs per freeze
  (`twview.cc:35`); `STakeStack` avoids that with a private silence component.
- M6. The `.qxa` runner and `action_roundtrip_test` are the only gates; `assert-meter`
  drives the LEGACY PULL, which gates on the *chain's* epoch that
  `invalidateRenderPath()` does not reach (CLAUDE.md "Level meters").

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
  (sec/beat, beats/bar); snapping has an unexposed subdivision; the tempo box and
  ruler "Set BPM" write the project directly, not through an action.

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

### D2 — Ticks in the content, frames everywhere else, one named conversion
`SMidiSequence` stores event times in **musical ticks (PPQ 960, integer)** — the
domain every reference DAW stores MIDI in (REAPER 960, Cubase 480, Logic 960,
Ardour 1920; Ardour ≤ 6 with samples + tempo map is the cautionary tale that
forced their 7.0 rewrite). The **window** (`SMidiCut`), the **placement**
(`SLink::startTime`), every automation lane and the whole engine speak **frames**
exactly as today. The single conversion is `twTempoMap::ticksToFrames /
framesToTicks` — a constant-tempo map object now (`usPerQuarter` int64, exactly
SMF's tempo unit, so the conversion is an exact rational: `frames = ticks ·
usPerQuarter · srate / (ppq · 10⁶)`), tempo *segments* later behind the same
interface (proposal 37). `tw/core/twdomains.h` gains a `TickPos` domain so mixing
ticks with frames is a compile error (proposal 18's rule).

Consequences, stated so they are not rediscovered: an `SMidiCut`'s
`getDuration()` in frames depends on the tempo, so a BPM change emits
`durationChanged` for every MIDI cut (→ `updateClip`, range invalidation) and
notes re-map onto the grid while **audio and clip placements do not move** — REAPER
"Beats (length, rate)" semantics with a time-locked position; a per-clip
`timebase` tag on `SLink` is reserved for "Beats (position)" later. Split/trim
arithmetic on a MIDI cut converts the frame offset to ticks through the map once
(exact rational), like `SCut` converts warped→source once.

Two of the three code studies recommended frames-now ("every persisted position is
a Fraction of frames; the tempo map does not exist; proposal 12 said sample-native
+ overlay"). Overruled on the industry evidence: recorded MIDI that does not follow
a tempo change is a defect users hit in the first hour, and the migration cost of
switching later is precisely Ardour's. The cost we accept instead is bounded to
`SMidiCut` (one class listens to `bpmTempoChanged`).

### D3 — No track kind; an instrument is a slot with a role; clip kinds mix
REAPER's model: any track holds audio and event clips; the instrument is
`SPluginChain` slot 0 carrying `isInstrument`; MIDI-out is one serialized
attribute pair (`midiOutPort`, `midiOutChannel`). Rejected: an explicit
`kind = audio|instrument|midi` (Cubase/Logic) — a second source of truth undo can
desynchronize, kind rules for folders/edit groups/`move-clip`, and old projects
would need a default. Rules: at most one instrument per track, always slot 0
(`insert-plugin` of an instrument descriptor goes to slot 0; a second is refused;
`reorder-plugin` cannot move an effect before it); an event clip on a track without
an instrument is **inaudible, not rejected** (like a take stack with
`activeTake = -1`); an audio clip on an instrument track **sums** with the
instrument (§4.3); the UI shows an instrument glyph and colour, derived.

### D4 — Class-1 continuity: reset + chase + pre-roll, and a run barrier
An instrument page frozen at a position that is not `lastEnd_` is a REPOSITION:
`reset()` (all notes off) → chase `stateAt(P − K)` (held notes with their
velocities, sustain, last CC values, bend, program) at offset 0 → pre-roll K frames
with events at their real offsets, output discarded → render the page. K = min(the
plugin's declared tail, one 4096 chunk) by default. Instruments need no upstream
pages for pre-roll, so it is plannable without a `planPage` override (plugins inv.
14). Effects stay reset-only. Rejected: capturing the plugin state blob per page —
`saveState()` per 65536-frame page per instance is a serialization of the *preset*,
not the voices (no format defines otherwise), MB-sized for a sampler, and VST3
`setState` is not RT-safe (F1, proposal 19's execution-class analysis).

**Run barrier** (F3/F4): pages of a class-1 component must come from one run.
`beginRun(pos)` — called by `RenderSession` at start and by `AudioEngine` on
seek/readahead jump, from a non-render thread, *before* demanding (F10) — walks the
project's class-1 processors and `invalidatePagesInRange(pos, INT64_MAX)`s their
taps and clears `lastEnd_`. Idempotent under any ordering (THREADING rule 4): a late
barrier costs one re-render, never a wrong page served as current. Rejected: a
`runId` stamp on pages treated as stale-but-servable — staleness is decided by
epoch in five places today; a second dimension is invasive. Barrier first; revisit
if re-render cost shows.

Determinism claims, precisely: render = ONE run from the range start ⇒ byte-`cmp`
across builds and processes by construction. Render vs playback are *not*
bit-identical (different run starts) — they never were for stateful inserts;
every PR body says so (CLAUDE.md "what was not gated"). What *is* gated: two
renders `cmp` equal; a seek-then-play capture case shows the chased note in the
first page after the seek.

### D5 — Automation is a curve snapshot consumed at freeze time; the fader moves post-FX
A `twAutomationCurve` (immutable sorted breakpoints in frames, `valueAt`,
`fillRamp(dst, P, n)`) is set on the consuming component under its mutex and read
once per page into a local (THREADING rule 2). Consumers: a new **`twGainStage`**
between the last tap and the rewire (the fader, mute-with-ramp, later pan) with a
per-sample ramp — exact, 65536 multiplies; and the slot processor, which turns
per-parameter curves into per-4096-chunk sample-offset `ParamValue` events
(value-at-chunk-start "chase" first, then breakpoints, then a dense 64-frame ramp
along continuous segments for plugins that do not interpolate). **The epoch is the
hash**: every automation edit range-bumps the *consuming component* (gain stage
exact range; processor `[a, ∞)` because it is class-1, F9); an app-side edit that
touches no component epoch is invisible to the scheduler by construction (F8, F10).

The fader moves from `twTrackMix` (pre-FX, F6) to `twGainStage` (post-FX) for
**every** track: an instrument's output must be under the fader, and post-insert
faders are what every reference DAW does. This is a golden-moving change only for
tracks with a *non-linear* insert at a non-unity fader; the in-repo fixtures are
linear, so the gate is byte-identity — with the LSB caveat named in P3's brief and
a re-freeze licensed only with a written justification.

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
renders with no playhead. A `MidiOutPump` follows the playhead atomic (F11) a small
window ahead, slices the track's event snapshots by timeline position (D1 — the
event data never enters the freeze model for MIDI-out) and hands `{dueHostTime,
port, bytes}` to a `MidiOutScheduler` std::thread (no Qt) through an SPSC ring;
CoreMIDI/ALSA-seq get driver timestamps handed off early, WinMM gets send-at-due-
time (±1 ms, honestly not gated). Alignment: `dueHostTime = hostTime(playhead) +
deviceOutputLatency − midiOutLatency − userOffset`, reusing
`meterLatencyFrames()`'s device→project mapping. Chase CC/PC/bend on locate,
optional note-on chase, all-notes-off + sustain-off on stop and on seek, panic
button. Backends behind a `MidiOutput`/`MidiInput` interface in `tw/devices`
mirroring `AudioBackend`/`AudioInput`: WinMM, CoreMIDI, ALSA sequencer, **capture**
(default under `--test-case`, `SMARAGD_MIDI_BACKEND=capture`), null. Rejected:
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
acceptance *demo*, never the gate.

### D8 — Persistence tolerance and the window interface land first, alone
Two prerequisites are pure app-model work, independently gateable, and benefit
every future object type: (a) the loader drops an **unresolvable clip link**, not
the containing track (M4), plus a `formatVersion` attribute readers warn on but
never refuse — the first feature that makes the cascade user-visible across builds
is this one; (b) `SClipWindow` — the window arithmetic `split/resize/duplicate/
take-*` need — extracted from `SCut` into `app/model` so those verbs dispatch on
the interface and `STakeStack` becomes a stack of windows. Rejected: an "event
mode" inside `SCut` — it pushes `if (isEvent)` into every reader/capture/preview
path of the most-tested class in the repo.

---

## 3. The app model

### 3.1 Objects — content / window / placement, the house pattern applied

```
SMidiSequence : SObject            CONTENT  (the SPlainWave analogue)
  events_        sorted vector<SEvent>, times in TICKS (PPQ 960)      D2
  ppq_ = 960, origin (smf | recorded | drawn), lengthTicks_
  snapshot()     shared_ptr<const SEventTable> — immutable, swapped under mutex()
  hasDuration()  = true (override; the base derives it from children)
  getDuration()  = lengthTicks mapped through the tempo map (frames)
  getRandomSource() = NULL; getRootComponent() = a private silence component (M5)
  contentKind()  = Event                                              (new SObject virtual)
  persisted INLINE:  <SMidiSequence …><events count=…><e …/></events></SMidiSequence>

SMidiCut : SObject, SClipWindow    WINDOW   (the SCut analogue; NOT an SCut mode)
  content_ SLink → SMidiSequence (+1 ref, exactly SCut::content_)
  srcStart (TickPos, exact rational), cutDuration (frames), loopLength (frames), stretch (Fraction rate)
  transpose (int semitones), velocityScale (double), channelOverride (-1 = keep)
  Q_PROPERTY Stretch, Transpose, VelocityScale
  snapshot(): SMidiCutSnapshot { window…, shared_ptr<const twEventSeq> framesSeq }
              — the FRAME-domain event sequence, rebuilt on any content/window/tempo edit
  mapTimelineToComponentPos: identity when looping, else + startOffset  (mirrors SCut::clipToReaderMap; no warp domain)
  listens to SProject::bpmTempoChanged → rebuild snapshot, emit durationChanged

SLink                              PLACEMENT (unchanged; `timebase` attribute reserved)
STakeStack                         unchanged shape; a stack of SClipWindow (D8b)
```

`SEvent` (model) is a fixed-shape typed record; unknown kinds round-trip verbatim:

```
struct SEvent {
  int64      t;        // ticks, sequence-relative
  uint16     kind;     // Note, CC, PitchBend, ChannelPressure, PolyPressure, ProgramChange, SysEx,
                       // Tempo, TimeSig, KeySig, Marker, Lyric, ChordSymbol, Articulation,
                       // StringFret, TrackerCell, Text; 0x8000+ = unknown/vendor
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
an `Articulation` event at the note's tick + `text`, and a per-note attribute bag
is reserved as `kind = NoteAttr` referencing the note by `(t, key, channel)`.

XML: one `<e>` per event, generic attribute names, `k` = a name for known kinds and
a hex number for unknown ones, sorted on write (diff stability, proposal 32).
Inline by default; an imported `.mid` is materialised inline on first save so
note data can never go missing the way a sample file can.

### 3.2 Track: no kind, three derived facts, two attributes
`STrack` gains `midiOutPort=''` / `midiOutChannel='-1'` (serialized) and three
derived, non-serialized helpers: `instrumentSlot()` (slot 0 with `isInstrument`),
`hasEventClips()`, `hasMidiOut()`. `trackChildWasAdded/Moved/Removed` and
`trackChildDurationChanged` route `contentKind() == Event` children into the
per-track **event clip set** (§4.2) keyed by `SLink*` — same slots, same key rule
(CLIP_MODEL "identity is the SLink pointer") — and NOT into the bus mixers, so no
dummy freeze per page per MIDI clip. Every event edit calls
`invalidateRenderPathRange(a, ∞)` on the cut (open-ended: the consumer is class-1,
F9) and that range must reach the **instrument tap** (via the processor's epoch),
not merely the track's mixers — the same "an `SPluginChain` is not an `SLink`
child" pitfall as plugin edits.

### 3.3 Automation persistence
`SAutomationLane` — a plain owner-held `QObject` (NOT an SObject; never an `SLink`
child): `{ParamRef target; Mode mode; sorted points {Fraction t; double v; Curve
c; double tension}; shared_ptr<const twAutomationCurve> snapshot}`. Owners and
their time domains:

| Owner | Lanes | Time | Travels with | XML |
|---|---|---|---|---|
| `STrack` | `self:Volume`, `self:Muted`, later `self:Pan` | timeline frames | nothing (arrangement time) | inline `<automation><lane target=… mode=…><p t= v= c=/>…</lane></automation>` |
| `SPluginSlot` | `param:<id>` (+ `name` for recovery) | timeline frames | the slot (survives `reorder-plugin`; dies with `remove-plugin`, whose inverse carries it) | inline in `<SPluginSlot>` next to `<state>` |
| `SCut` / `SMidiCut` | `cut:Gain` (audio), `cut:VelocityScale`, `cut:Transpose` (event) | clip-relative frames | the window (every placement / take / asset of it) | inline in the cut element |
| `SLink` | — | — | — | **deferred** (no identity, M2) |

`ParamRef` spaces: `self:<Q_PROPERTY>` (Volume, Muted; Pan only after the sink is
stereo — M1), `param:<id>` on a slot (value in the plugin's host-facing domain —
normalized for VST3, plugins inv. 26 — the same domain `set-plugin-param` uses),
`cut:<prop>`. `Stretch` is not automatable (it changes duration).

Older builds ignore inline `<automation>`/`<events>` children of known elements
(the loader orders on `<SLink>` children only) — that is why they are inline and
not top-level objects.

### 3.4 Actions (the scripting API grows; verbs are ABSOLUTE like `set-pitch`)

| Verb | Attributes (name = default) | Notes |
|---|---|---|
| `insert-midi-clip` | `trackPath`, `timePos`="0", `duration`="0" (0 = one bar), `name`="" | sequence + cut + link; inverse = remove-clip |
| `import-midi-file` | `trackPath`="", `filePath`, `timePos`="0", `mode`="tracks\|channels\|merged", `newTracks`="1" | one sequence per SMF track; PPQ rescaled to 960; SMF tempo → `set-tempo` if the project is empty, else warned; composite (atomic undo); path via `SFilePathRef` |
| `export-midi-file` | `clip`="" or `trackPath`="" or whole project, `filePath`, `type`="1" | not undoable; gate = `assert-midi-file` |
| `add-note` / `remove-note` | `clip`, `tick`, `dur`, `key`, `velocity`="100", `channel`="0", `releaseVelocity`="64", `take`="-1", `broadcast`="1" | note addressed by `(tick, key, channel)` |
| `set-notes` | `clip`, child `<n tick= dur= key= velocity= channel= …/>` — absolute new state of an addressed set, `take`, `broadcast` | THE batch verb: a piano-roll drag of N notes = one undo step; `mergeKey` = clip + selection hash |
| `add-event` / `remove-event` / `set-events` | `clip`, `kind`, `tick`, `channel`, `a`, `b`, `f`, `text`, `blob` | CC / bend / metadata; `set-events` batches CC drawing |
| `quantize-notes` | `clip`, `grid`="1/16", `strength`="1.0", `swing`="0", selection children | a `set-notes` composition; the inverse is the previous state |
| `set-midi-cut` | `clip`, `transpose`="0", `velocityScale`="1", `channel`="-1", `take`, `broadcast` | window fields go through the generalized `resize-clip` (D8b) — no second window verb |
| `set-tempo` | `bpm` | replaces the two direct writes (U3); later `set-tempo-segment` |
| `insert-plugin` | + `isInstrument`="false" in the descriptor | no new verb; instrument ⇒ slot 0, refused if one exists |
| `set-track-midi-output` | `trackPath`, `port`="" (portable device NAME), `channel`="-1" | per-machine ids resolve in `SSettings`, like the audio input device |
| `add-automation-lane` / `remove-automation-lane` | `owner` (trackPath \| clip \| trackPath+`slotIndex`), `target`, `mode`="read" | inverse carries the whole point list |
| `set-automation-mode` | `owner`, `target`, `mode`="off\|trim\|read\|touch\|latch\|write" | |
| `add-automation-point` / `move-automation-point` / `remove-automation-point` | `owner`, `target`, `time`, `value`, `curve`="linear", (`toTime`, `toValue`) | points addressed by old `(time, value)` |
| `set-automation-points` | `owner`, `target`, `from`, `to`, child `<p t= v= c=/>` | the batch/coalescing verb (curve drawing, Touch/Latch/Write commit); `mergeKey` = owner + target |
| `set-track-volume` on a track with a Read lane | unchanged attributes | becomes a `set-automation-points` at the locator, else history and lane disagree |

Test-harness verbs (testkit): `assert-midi-events` (count / kind / at / key /
velocity / contains on a cut's snapshot), `assert-midi-file` (export gate),
`assert-automation-value` (`owner`, `target`, `time`, `value`, `tolerance`),
`assert-midi-out` (against the capture MIDI backend: `port`, `count`, `at`,
`kind`, `key`, `channel`, `tolerance`="4096" frames), `dump-midi-capture`,
`assert-event-editor` (off-screen dock `describe()` + `grabPng`), `virtual-key`
(computer keyboard → note event insertion at the locator), `drag-automation-point`
(the `drag-clip-edge` twin), `automation-write-tick` (one live tick of a Touch/
Latch/Write pass, the `slip-clip` shape). Every verb gets a row in
`action_roundtrip_test`.

### 3.5 Serialization and versioning
New classes self-register by static initializer (OBJECT-lib rule); a new slice
`objects/midi` at the DAG rank of `objects/cut` (`check_layering.py` rows:
`objects/midi → {actions, model, persistence}` + engine `tw/events`, `tw/core`);
`objects/track` must NOT depend on `objects/midi` — the track consults MIDI-ness
only through `SObject::contentKind()`. `<SProject formatVersion='2'>` (D8a);
`kScannerVersion` 1 → 2 for the plugin cache (P1 adds descriptor fields).
Portable references: `.mid` import paths through `SFilePathRef`; MIDI device ids
are machine-local and live in `SSettings` keyed by a portable name.

---

## 4. The engine

### 4.1 `tw/events` — a new leaf module (core only), no place in the dataflow DAG
- `twEventKind`, `twEvent` (engine twin of `SEvent`; times are `ClipPos` frames at
  project rate — the SMidiCut snapshot converts ticks once through the tempo map),
  `twEventSeq` (immutable sorted vector + `slice(a, b)` + `stateAt(P)`),
  `twTempoMap` (constant tempo now: `usPerQuarter`, `ppq`, `numerator/denominator`;
  `ticksToFrames(TickPos, srate) → Fraction`, `framesToTicks`; segments later behind
  the same calls), `TickPos` in `tw/core/twdomains.h`, `twSmf` (SMF type 0/1
  reader/writer, PPQ rescale, meta events → `Tempo/TimeSig/KeySig/Marker/Lyric/
  Text` kinds, unknown meta preserved), and `twAutomationCurve` (proposal 12's
  second lane kind: sorted `{frame, value, curve, tension}`, `valueAt`, `fillRamp`).
- Unit-tested in `events_test` (round-trips, `stateAt` against brute force,
  tick↔frame exactness at 44.1 k/48 k/96 k, curve evaluation vs closed forms).

### 4.2 `twEventClipSet` — the event twin of `twTrackMix`'s clip list (tw/mix)
Not a `twComponent` (like the processor is not): it produces no pages.
`insertClip/updateClip/removeClip/setClipMuted(key = SLink*, startTime, duration,
resolver → shared_ptr<const twEventSeq> + MapPosFn)` return `twEditRange`;
`collect(startPos, len, out)` yields (a) the **chase set** at `startPos` and (b)
events in `[startPos, startPos+len)` with page-relative offsets, clamped to each
clip window — with the event twin of the clip-end-bleed clamp: **note-offs are
synthesised at the clip end for notes still held, and note-ons before the window
are only ever issued through the chase**. Loop and slip go through the cut's map
like audio (POSITION_DOMAINS rules 1-3). Consumers: the instrument slot processor
(§4.3) and the MIDI-out pump (§4.6). Owned by `STrack`, one per track (events are
not per bus).

### 4.3 The instrument slot (tw/plugins) — generator modes of the existing processor
Design option A (engine study): the instrument is **slot 0 of the existing
`twPluginChain`** with a `twPluginSlotProcessor` in a generator mode; the head tap
per bus is otherwise unchanged. Everything from proposal 08 (descriptor, state
chunk, params, Missing placeholder with the *declared* layout, `setFactory`
re-resolution, the all-bus cache, `SPluginSlot` serialization) is reused verbatim.
Rejected: a `twInstrumentTrackMix` component (would grow its own processor/tap
split) and "instrument replaces the track's content component" (changes the graph
shape every invalidation walk assumes).

- Mapping table rows (plugins inv. 16 amended): `0→N on N buses` DirectGen (one
  instance); `0→1 on N buses` MonoSpread; `0→2 on 1 bus` fold (MonoFold's average);
  `0→M, M>N` outs `0..N-1` to this track's taps, `N..` into `Output.bus[M]` for
  **aux taps** (§5.4). Under proposal 35 (§9.1) "N buses" becomes "one N-channel
  page" and DirectGen renders one wide page — the table survives, the tap widens.
- **Pass-through sum** (D3): the instrument tap keeps its audio input plug (the
  bus's `twTrackMix`) and adds it to the instrument output, so audio clips on an
  instrument track are heard. The chain wiring rule that would leave a 0-input head
  unwired (F7) is replaced by "the head tap always has one input; the processor
  decides whether the plugin sees it". Documented, testable (audio + MIDI clip on
  one track → both audible), and it keeps the graph shape (`bumpRenderChainEpoch*`
  walks unchanged).
- Events reach the processor per page: `eventClipSet.collect(startPos, len)` →
  per-4096-chunk slice with rebased offsets → `process(in, outBuses, n, events,
  eventsOut, ctx)`; the processor also merges the UI `setParam` ring (offset 0) and
  the automation events (§4.5) into ONE sorted list per chunk (plugins inv. 5
  amended: "events are chunk-relative; the processor rebases").
- Continuity: D4 (`contiguous ⇒ process; else reset + chase + pre-roll K`).
  `kCacheEntries` 2 → 4 for instruments (thrash with several taps). Instrument
  **bypass = silence but keep feeding events** (never the effect short-circuit, or
  un-bypass resurrects stale voices). `tailFrames()` sizes pre-roll and defines a
  note-driven project end. `reportedLatency()` stays reported-not-compensated
  (surfaced in the UI for instruments; PDC is a separate proposal).
- Instruments are **freeze-path only**: `calcOutputTo` on an instrument tap
  returns silence and logs once (the legacy pull is positionless and on proposal
  20's retirement list). Preview freezes forward the upstream page (plugins inv. 6),
  so an instrument track's audio *waveform* preview is empty — the MIDI clip's
  thumbnail is app-side (§6.1); a level envelope can later be read from cached
  playback pages by position (34's pattern). Do NOT render plugins at 1 kHz.
- `twProcessContext {position, playing, tempoBpm, ppqPos, tsNum/tsDen,
  validFlags}` per call (F5): the tap's `renderPos_` + the tempo map. Reset the
  plugin's position at every reposition.

### 4.4 The run barrier (tw/schedule + consumers) — D4
`CaptureRevalidator::beginRun(pos)` (or a free function over the app's list of
class-1 processors) is called by `RenderSession::start` and by
`AudioEngine::seekTo` / the readahead jump path **before** any demand; it
`invalidatePagesInRange(pos, INT64_MAX)`s every class-1 tap and clears
`lastEnd_`. Registered from the app (every `SPluginSlot`'s processor via `STrack`)
because the engine has no project-wide list. Order-independent by construction:
if a worker is mid-render at that position the verify-at-publish self-staleness
check re-stales it (schedule inv. 8), so the wrong page is never *current*.

### 4.5 `twGainStage` and automation consumption — D5
- `twGainStage` (tw/mix): 1-in/1-out per bus between the last tap and the rewire;
  scalar `gainDb` (the fader) × optional `twAutomationCurve` (Volume, dB-linear in
  fader space, `sfadercurve.h`) × mute (0 with a 1–2 ms ramp; NOT the parent's
  plug-nulling, which stays the *structural* mute for solo rules). Class ∞ pure ⇒
  range invalidation is EXACT: an edit on `[a, b)` re-renders exactly those pages.
  `twTrackMix::trackGainDb_` becomes 1 (kept one release for A/B, then removed).
- Processor: `setParamCurves(map<paramId, shared_ptr<const twAutomationCurve>>)`
  under `mutex_`; per chunk: value-at-chunk-start `ParamValue` (the parameter
  "chase" — pages render out of order), one point per breakpoint inside the chunk,
  a 64-frame dense ramp on continuous segments; `automationEpoch_` enters
  `stampNow_nolock()` (inv. 15). Every lane edit → `invalidateRenderPathRange(a, ∞)`
  from the `STrack` (pluginui inv. 6 still applies).
- Per-clip gain envelope (`cut:Gain`): applied to `childPage` before `mixFrom` in
  `twTrackMix::freezePage_nolock` — the clip window's `WarpedPos` domain through the
  same `clipToReaderMap`, so it trims/slips/loops with the clip. `cut:VelocityScale`
  / `cut:Transpose` are applied when the SMidiCut snapshot is built (they are
  event transforms, not audio).

### 4.6 MIDI devices and the out pump (tw/devices + app) — D6
`MidiOutput { open(id) / close / listPorts / createVirtualPort(name) /
send(bytes, n, hostTimeNs = 0) / latencyNs() }`, `MidiInput { … setCallback(bytes,
n, hostTimeNs) /* device thread, no Qt */ }`, `createMidiOutput()` selected by
`SMARAGD_MIDI_BACKEND = winmm | coremidi | alsaseq | capture | null | default`.
`MidiOutScheduler`: one std::thread, SPSC ring of `{dueHostTimeNs, port, 3–N
bytes}`, `WaitableTimer`/`nanosleep` at 1 ms granularity (`timeBeginPeriod(1)` on
Windows). `MidiOutPump` (app, main-thread `QTimer` like `pumpMeters`): reads the
playhead atomic, walks tracks with `midiOutPort`, `collect`s their event clip set
over `[playhead + lookahead, +window)`, converts channel remap, enqueues; on
start/seek: chase (CC/PC/bend always, note-ons optional per settings); on stop /
seek / panic: sustain-off + all-notes-off per used channel. Renders contribute
silence to MIDI-out (there is no playhead). The capture backend records
`{hostTime → project frame, bytes}` for `assert-midi-out`.

---

## 5. The plugin ABI extension (format-agnostic core, three backends)

### 5.1 One header, one overload, no format types
`tw/plugins/twpluginevents.h`: `twEventKind {NoteOn, NoteOff, NoteChoke, NoteEnd,
NoteExpression, PolyPressure, ControlChange, PitchBend, ChannelPressure,
ProgramChange, ParamValue, ParamGestureBegin/End, ParamMod, Midi1, Sysex,
Transport}`; `twEvent {uint32 time (chunk-relative, sorted asc); kind; flags
(IsLive, DontRecord); port; channel; key; noteId; paramId; double value, value2;
data/size (sysex arena); bytes[4]}`; `twEventList` (host-owned, sized in
`prepare()`), `twEventOut` (plugin → host sink, overflow counted and dropped),
`twProcessContext`. `twPlugin` gains `capabilities()` (`acceptsNotes, emitsNotes,
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
  frame grid via `try_push` (event-out plumbing, arp → sine routing, MIDI-out).
- `twtestvst3.cpp` `TestSine`: SPLIT component/controller, `Instrument|Synth`, 0
  in / stereo out, one kEvent in bus, reads `inputEvents`, honours `noteId`,
  **ignores an unactivated event bus** (a host that forgets `activateBus(kEvent…)`
  fails the level assert), Gain via `inputParameterChanges` honouring
  `sampleOffset`, `IMidiMapping` CC7 → Gain.
- `twNativeInstrument` (tw/plugins builtin, `format="tw"`, uid `tw.native.303`):
  monophonic; params {cutoff, resonance, envMod, decay, accent, waveform, slide};
  velocity ≥ 100 = accent; overlapping notes = slide; deterministic (dsp inv. 2);
  present in every build, every worktree.

### 5.4 Multi-output (designed here, built in P9)
`Output.bus[M]` on the processor + a tap `(processor, busIndex ≥ N)` placed as the
*content component* of a **return track** IS the multi-out design with zero new
engine types; missing are the app model (a return track object referencing a
`(track, slot, out)` triple, serialized like `pluginChainId`), the processor cache
sized to `nOuts + slack`, and UI. Named here so P3 does not paint over it.

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
Grid: reuse `STimeGridSpec`, add divisions to `SSnapSpec`, consume the tempo map.

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
`drag-automation-point`/`virtual-key`; a generic seam is P9.

---

## 7. Threading and invariants (new or amended; each phase's brief names its subset)

Threading, unchanged in kind: event tables, cut snapshots and curve snapshots are
immutable and swapped under the owner's mutex (rule 2); the scheduler holds a
snapshot for the duration of a freeze; the MIDI scheduler and the capture MIDI
backend are Qt-free std::threads publishing through rings/atomics (rule 1); the
MIDI-out pump and every UI indicator run on the main thread off `meterTick`/a
`QTimer`; the run barrier is issued from a non-render thread before demanding
(rule 3 + F10); every fix stays order-independent (rule 4). New thread in the
inventory table: `MidiOutScheduler`.

| Contract | Amendment |
|---|---|
| CLIP_MODEL | fourth kind of child: same placement/window layers, engine view = event clip set entry, not `ClipEntry`; note-offs synthesised at the clip end; take stacks over `SClipWindow` |
| POSITION_DOMAINS | new domain row **Ticks** (musical; `SMidiSequence` events; only `twTempoMap` converts) and rule 7: "an `SMidiCut` speaks frames on every side the track sees; the tick→frame conversion happens exactly once, in the cut's snapshot build" |
| FREEZE_PROTOCOL | class-1 run protocol paragraph: reposition = reset + chase + pre-roll; run barrier before demanding; state blobs per page are NOT captured |
| plugins/CONTRACT | inv. 5 (chunk-relative events), 6 (instrument preview = silence), 16 (generator rows), the discontinuity paragraph → "reset + pre-roll + replay"; new: note ids, one sorted list / one dialect, VST3 event bus activation, instrument bypass keeps events, `automationEpoch_` in the stamp |
| mix/CONTRACT | `twGainStage` between chain and rewire is THE fader; `twTrackMix` gain retired; per-clip gain envelope applied pre-mix; `twEventClipSet` mirrors the ClipEntry rules |
| schedule/CONTRACT | `beginRun` barrier semantics; class-1 edits invalidate `[a, ∞)` |
| model/CONTRACT | `contentKind()`; inline `<events>`/`<automation>` are the sanctioned non-SLink payloads; the loader ignores them for ordering; unresolvable clip link → drop the link |
| persistence/CONTRACT | `formatVersion` (warn, never refuse); missing-clip-target policy |
| track/CONTRACT | event clip set ownership + sync (same key rule as clips); automation lanes are owner-held, never `SLink` children; instrument slot rules; Volume/Mute reach the post-FX gain stage |
| cut/CONTRACT | `SClipWindow`; "windowed verbs never `dynamic_cast<SCut*>` for arithmetic the interface provides" |
| timeline/CONTRACT | inv. 2 names `contentKind()`; inv. 5 wording includes automation lanes; the event editor is a selection follower (inv. 8/9) |
| testkit/CONTRACT | `SMARAGD_MIDI_BACKEND=capture` default under `--test-case`; the test instruments; the `assert-meter` legacy-pull caveat extends to event and automation edits |
| THREADING | inventory row `MidiOutScheduler`; MIDI-out at play time |
| docs/ACTIONS.md | every verb in §3.4 |

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
| **P0a** Persistence tolerance + `SClipWindow` | loader drops an unresolvable clip LINK; `formatVersion`; `SClipWindow` extracted; verbs + take stacks dispatch on it | — | existing suite green; new `qxa` `load_unknown_object_survives`; every windowed verb round-trips unchanged; goldens byte-identical |
| **P0b** `tw/events` leaf | `twEvent/Seq`, `twTempoMap`, `TickPos`, `twSmf`, `twAutomationCurve`, `events_test` | — | unit tests (SMF corpus round-trip, `stateAt` vs brute force, tick↔frame exact at 3 rates); layering (leaf, core-only) |
| **P1** Event clips in the model | `SMidiSequence`, `SMidiCut`, `contentKind()`, `objects/midi`, verbs (§3.4 rows 1–8), `twEventClipSet` + `STrack` sync, thumbnail renderer, `assert-midi-events/-file`, `set-tempo` | P0a, P0b | `qxa`: import → save → load → export `cmp`; split cuts a straddling note; stretch/loop/slip/take verbs; BPM change re-maps notes and moves no audio; undo verify; old-build tolerance |
| **P2** Plugin ABI events + in-repo instruments | `twpluginevents.h`, `process()` overload, capabilities, tail, aux-out discovery; CLAP/VST3/AU translation; generator modes; reset+chase+pre-roll; `tw.test.clap.sine/arp`, VST3 `TestSine`, `twNativeInstrument`; scanner v2 | — (parallel with P1) | `plugins_test`: notes → sine RMS + frequency per format; mid-chunk gain step lands at the offset; unactivated-bus teeth; effect goldens byte-identical; the app hosts nothing yet |
| **P3** Instrument tracks + fader move | instrument slot rules, head-tap pass-through sum, event feed, run barrier at render start/seek, `twGainStage` (fader post-FX), browser filter, strip row, head "I" | P1, P2 | `qxa`: `instrument_sine_render` (freq/RMS per note), `instrument_render_determinism` (`cmp` two renders), `instrument_seek_continuity` (capture backend, chased note in the first page after seek), mixed audio+MIDI track, edit reaches the render, native 303 `cmp`; fader move byte-identical at 0 dB and for the linear fixtures; `repeat_test` sweep (arp→instrument in-app is P9) |
| **P4** Event editor (piano roll) + virtual keyboard | `main/eventui`, dock, view base + kind registry, piano roll + velocity/CC lanes, time axis link, grid/quantize, `virtual-key`, `assert-event-editor`, `describeTrackHead` | P1 (P3 for audible checks) | action round-trips + verify-undo for every editor op; off-screen `describe()`; PNGs; `virtual-key` → render → `assert-audio-frequency` |
| **P5** Automation model + engine | `SAutomationLane`, `ParamRef`, verbs (§3.4 rows 12–17), curve snapshots, gain-stage ramps, processor param events + `automationEpoch_`, clip gain envelope, range invalidation | P0b, P3 | `qxa`: `automation_volume_ramp` (per-second RMS follows the curve; render `cmp`), `automation_plugin_param` (test plugin gain step at a mid-page offset), `automation_clip_gain`, `assert-automation-value`; goldens without lanes byte-identical |
| **P6** Automation UI | sub-lanes, painting, gestures, picker, head mode button, Touch/Latch/Write recorder committing one action, plugin-gesture punch-in, clip envelope overlay, fader shows the read value | P5 | `drag-automation-point` + `automation-write-tick` cases; `assert-lane-alignment` over automation lanes; head `describe()` at three densities |
| **P7** MIDI output | `tw/devices` MIDI interfaces + WinMM/CoreMIDI/ALSA-seq/capture/null; scheduler thread; out pump; `set-track-midi-output`; Options MIDI page; chase; panic | P1 (P3 optional) | `qxa` `midi_out_capture` (notes at expected frames ± 4096; chase after seek; all-notes-off on stop); goldens unaffected; the ±1 ms WinMM jitter is stated as NOT gated |
| **P8** Live MIDI input + recording *(outline; gated on proposal 21 P1)* | `MidiInput` backends, event ring → processor merge point, computer keyboard as input, record to clip with takes | 21-P1, P3, P7 | to be written when 21-P1 lands |
| **P9** Follow-ups *(outline)* | multi-out → return tracks; tempo segments (proposal 37); MIDI FX chain (event-out routing between slots); score/tab/tracker views; generic `widget-gesture` seam; PDC | P3/P5 | own proposals |

Parallelism: P0a ∥ P0b ∥ P2 at the start; P1 after P0; P3 after P1+P2; P4 ∥ P5
after P3; P6 after P5; P7 after P1 (independent of instruments). Critical path:
P0 → P1 → P3 → P5 → P6.

---

## 9. Dependencies on other proposals

### 9.1 Proposal 35 — configurable multichannel signal flow (on `feat/multichannel`)
35 replaces "N buses = N mono component instances" with N-channel pages
(`twOutputPage::channels`, `getOutputChannels()`, `renderPageWide`). The
instrument work is written to survive both worlds: **P2/P3 target whatever main
has when they start.** Pre-35: the processor renders all outputs, one tap per bus
(today's split); post-35-B4: the head tap is a wide component that reads the
processor's per-channel outputs into one page. The generator rows of the mapping
table are unchanged either way. Until 35-B5 lands the sink is mono, so P3's audio
assertions read channel 0 only and **never assert `L != R`**. A stereo instrument
becoming audible in stereo is a 35 deliverable, not a 36 one. Pan automation
(`self:Pan`) is unlocked by 35-B5 and is explicitly out of P5.

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
| The fader move (D5) shifts goldens for non-linear inserts | Only linear fixtures exist; the P3 gate is byte-identity; any diff needs a written justification and a re-freeze commit |
| Chase alone restarts envelopes on pads (audible after a seek) | Pre-roll K subsumes it for notes started inside the window; K per slot; the seek case is a *playback* property and is capture-tested, not `cmp`-tested |
| Run barrier costs a re-render of the readahead window on every seek | Same order as today's whole-track bump on a param edit; measure in P3; the `runId` alternative is written down |
| Ticks-in-content (D2) couples `SMidiCut` duration to the tempo | One class listens to `bpmTempoChanged`; `durationChanged` reuses the existing sync; a `timebase` tag is reserved for placements |
| VST3 CC → `IMidiMapping` per CC per plugin is per-format special casing | Confined to the backend; the split-controller `TestSine` gates it |
| A MIDI project does not open in older builds | D8a lands FIRST as its own PR; documented in the persistence CONTRACT |
| `sstdmixerview.cpp` grows again | Automation lane painting and gestures go into new files; the event editor is a new module |
| WinMM has no timestamps and no virtual ports | Send-at-due-time in the scheduler thread; virtual ports offered only where the OS supports them; jitter reported as not gated |
| Legacy pull path (`assert-meter`) does not see event/automation edits | Same caveat as gain today; documented; disappears with proposal 20 |
| Split VST3 component/controller path has never run against a real host in this repo | `TestSine` IS that fixture; `vst3_probe` for triage |

## 11. Open questions (for the requester before P3)

1. Head-tap pass-through sum (audio clips audible on an instrument track) — REAPER
   overwrites channels 1/2 with a 0-input synth's output; Cubase/Logic disallow
   audio regions on instrument tracks. Summing is the least surprising for a
   kind-less track. Confirm.
2. Note-on chase on locate: default ON (Cubase-like) or OFF (REAPER's default is
   ON for MIDI hardware, Live never chases notes)? Proposed: ON for instruments
   (pre-roll makes it inaudible), OFF for MIDI-out unless the setting says so.
3. Trim/Read as the default automation mode (REAPER) vs Read (Cubase/Logic/S1).
   Proposed: Trim/Read — the fader value stays meaningful when a lane exists.
4. Whether split cuts a straddling note (Logic/S1/Live) or keeps it whole with
   overhang (REAPER default). Proposed: cut; overhang as a later preference.

## 12. Deliberately not done here
Tempo segments/ramps (37); return tracks / multi-out UI (P9); MIDI FX chains
between slots (P9); score/tab/tracker views (P9); MPE editing; MIDI clock/MTC;
external-instrument audio return; PDC; placement-scope envelopes (32); pan
automation (35-B5); live input/monitoring/recording (21 + P8); metronome engine.

## 13. Adversarial review
*(filled in by the review pass — findings, and what changed in response.)*

## 14. Glossary (ours → the references)

| Smaragd | REAPER | Logic | Cubase | Studio One |
|---|---|---|---|---|
| event clip (`SMidiCut` over `SMidiSequence`) | MIDI item / take source | MIDI region | MIDI part | instrument part |
| shared content (one sequence, many cuts) | pooled MIDI item | alias | shared copy | shared copy |
| clip modifiers (transpose, velocity scale, channel) | take pitch / item props | region parameters | MIDI modifiers | part inspector |
| instrument (slot 0 with a role) | VSTi in the FX chain | software instrument | VST instrument | instrument |
| track MIDI output (`midiOutPort/Channel`) | MIDI hardware output | External Instrument | MIDI track output | External Instrument |
| automation lane (track, timeline time) | track envelope | track automation | track automation | track automation |
| clip envelope (window, clip time) | take envelope | region automation | part controller lane | part automation |
| Trim/Read · Read · Touch · Latch · Write | Trim/Read, Read, Touch, Latch, Write | Off, Read, Touch, Latch, Write, Trim | Read/Write + Touch/Auto-Latch/Cross-Over | Off, Read, Touch, Latch, Write |
| tempo map (constant now) | tempo envelope | tempo track | tempo track | tempo track |
| chase | "Chase MIDI note-ons" | chase settings | Chase Events | chase |
