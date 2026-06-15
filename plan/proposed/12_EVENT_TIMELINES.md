# 12 — Event Timelines (automation, envelopes, tempo/meter, MIDI)

**Status:** Design only (2026-06-15). No code. This proposal exists to settle the
*shape* of a feature family the user named — sample volume envelopes, parameter
automation, per-clip automation, a tempo/time-signature track, and MIDI — before
any of it is built.

The user's framing: *"this all rings 'events' — should each object carry a
dictionary of timelines (parameter automation, a MIDI track, …)?"* This document
**challenges and refines** that instinct rather than accepting it wholesale.

---

## 1. The five use cases

| # | Use case | Data shape | Time domain | Consumer |
|---|----------|-----------|-------------|----------|
| A | Audio sample **volume envelope** | continuous curve (gain vs. time) | clip-local | per-sample gain in the reader/grain chain |
| B | **Track / plugin parameter** automation | continuous curve | arrangement | writes a named scalar param each block |
| C | **Per-clip** automation | continuous curve | clip-local | same as B, scoped to the placement |
| D | **Tempo / time-signature** track | tempo: piecewise curve; meter: discrete structure | project-global | defines beats↔samples mapping |
| E | **MIDI** | sparse discrete typed events | clip-local (usually) | an instrument that synthesises audio |

The single word "events" hides **three different data shapes**: continuous
control curves (A, B, C), a global coordinate system (D), and discrete typed
event streams (E). That is the crux of the challenge below.

## 2. What the current model already gives us (hard constraints)

- **Time is samples, period.** `offset_t`/`length_t` are 64-bit sample counts at
  project rate (`tw303a/include/twcomponent.h:14,19`). There is **no musical
  time** anywhere. Tempo is a single scalar `SProject::bpmTempo_`
  (`main/include/sproject.h:119`); there is no meter and no tempo map.
- **Content and placement are deliberately separate.** An `SObject` is shared,
  reference-counted content (`nRefs_`, the asset registry). An `SLink` is *one
  placement* of that content at a `startTime_` in a container (`STrack`)
  (`main/include/slink.h:23-75`). The same sample can be placed many times.
- **Parameters are already reflectable.** `Volume/Pan/Delay` on `SObject` and
  `Stretch/PitchCents` on `SCut` are `Q_PROPERTY`s
  (`main/include/sobject.h:54-60`, `main/include/scut.h:76-77`) — addressable and
  settable *by name* via `QObject::property()/setProperty()` without knowing the
  concrete type.
- **The lock-free pattern automation needs already exists.** `SCutSnapshot` +
  double-buffered reader state (`main/include/scut.h:42-70`) is exactly how the
  UI thread hands the audio thread an immutable, swap-on-edit view. Automation
  lanes want the same shape.
- **An edit/undo/scripting spine already exists.** `SAction` with
  `apply→inverse`, `writeXml/readXml`, and `mergeKey/mergeWith` coalescing
  (`main/include/saction.h`). Drawing a curve = a coalesced stream of point
  edits; this fits with no new machinery.
- **Serialization is manual XML** (`serialize(QTextStream&)` out,
  `instantiateFromDomElement` + a registry in) — plus a generic JSON escape hatch
  `SProject::properties_` (a `QVariantMap`). New persisted structures are child
  elements with their own attributes.

## 3. The challenge — where "a dict of event timelines on every object" breaks

**3.1 "Events" conflates two incompatible value semantics.**
A volume envelope is a *continuous function of time* defined by breakpoints + an
interpolation rule (step / linear / curved). MIDI is a *sparse stream of discrete
typed messages*. Forcing both into one "event" type yields either a fat union or
interpolation bolted onto a model where it is meaningless. They need different
storage, editors, and audio-thread evaluation.
→ **Share the *container*, split the *lane kind*.**

**3.2 "Every object" collides with the content/placement split.**
Because objects are *shared*, where a timeline hangs decides what it *means*:
- a sample's **intrinsic** volume envelope (A) should travel with the content →
  it belongs on the `SObject`/`SCut` window;
- **track / clip-placement** automation (B, C) must **not** be shared across
  every placement of the same content → it belongs on the `SLink` (placement) or
  `STrack`, in arrangement time.

So it is not "every object gets a dict." It is *timelines attach at the right
**scope** — content, placement, or project — chosen by whether the data should
move with the content or with the placement*. Same mechanism, three legal owners.

**3.3 Tempo/meter is not a peer timeline — it is the coordinate system.**
Until it exists, "beats" do not. A tempo map is a monotone function
samples↔beats; the meter map gives bar structure. MIDI quantize, bars/beats
display, and tempo-synced automation all sit on top of it. Modeling it as "just
another event timeline on the project object" inverts the dependency.

**3.4 MIDI is a producer, not just data.**
A MIDI lane is inert without an *instrument* that consumes events and synthesises
audio (the tw303a synth is the natural first consumer). "MIDI support" is two
projects: (1) the event lane + editor + I/O, and (2) note scheduling + voice
allocation on the audio thread. The "events on an object" framing captures (1)
and hides (2), which is the larger effort.

**Verdict:** the *backbone instinct is right* — a uniform, reusable timeline
abstraction is the correct spine. Keep it, with three refinements: **two lane
kinds (not one event type), three attach scopes (not flatly every object), and
tempo/meter as a privileged coordinate system (not a peer).**

## 4. The refined concept

**One backbone, three scopes, two lane kinds.**

```
Timeline                       // ordered, addressable container; stable IDs for lanes & points
 ├─ ControlCurve  lane         // continuous: sorted breakpoints {t, value, interp}; binary-searchable
 └─ EventStream   lane         // discrete:   sorted typed events {t, kind, payload}  (MIDI, markers, meter)

attach a Timeline at the scope that matches the meaning:
   SObject   (content scope)   → intrinsic envelopes that travel with the content     [A]
   SLink     (placement scope) → arrangement-time automation of a placement / clip     [B, C]
   SProject  (project scope)   → tempo map, meter map, MIDI tracks not tied to a clip  [D, E]
```

Connective tissue, each piece reusing something that already exists:

- **Addressing (`ParamRef`).** A `ControlCurve`'s target = `{owner scope,
  property-name}`, resolved through `QObject::property()/setProperty()`. Track
  volume = `{track,"Volume"}`; stretch = `{cut,"Stretch"}`. "Any plugin
  parameter" comes nearly free **if** plugin params are exposed as `Q_PROPERTY`s
  — make that the contract for automatable parameters (see Decision 5.2).
- **Audio-thread evaluation = the `SCutSnapshot` pattern.** Each lane publishes an
  immutable sorted array; the UI thread rebuilds + swaps; the audio thread
  binary-searches it lock-free. No new concurrency model — the one just hardened
  for WASAPI/`SCut` carries over verbatim.
- **Two evaluation rates, chosen per use case.**
  - *Control-rate* (evaluate once per block, write the scalar param, smooth to
    kill zipper noise) is simple and matches today's "set a scalar" model — good
    enough for B and C.
  - *Sample-accurate* is only needed where clicks bite: the **volume envelope (A)**
    is best applied as a per-sample multiply *inside the reader/grain chain*,
    which is localized and leaves the rest of the graph untouched.
- **Edits are `SAction`s.** Add/move/delete breakpoint, draw curve (coalesced via
  `mergeKey`), insert MIDI note — all undoable/scriptable for free. The timeline
  types therefore need stable IDs for lanes and points so actions can address
  them.

## 5. Decisions to settle before build

**5.1 Time storage: sample-native overlay vs. beat-native reflow.**
- **Option A (recommended first): sample-native + tempo overlay.** All material
  stays in `offset_t` samples; the tempo map is an *overlay* used only for
  display, quantize, and beat-relative features. Editing tempo does **not** move
  existing audio. Simplest; non-invasive; lets D and E land without rewriting the
  arrangement.
- **Option B: beat-native.** Musical material is stored in beats and resolved to
  samples through the tempo map; tempo edits reflow everything. Powerful, but
  every edit and every render must go through the map — a deep change to the
  sample-native engine.

**5.2 Scope of "any plugin parameter."**
- Make `Q_PROPERTY` the universal contract for automatable params (uniform, free
  reflection, aligns with the `SAction` name-keyed model), **or**
- introduce a parallel named-parameter registry for params that cannot be
  `Q_PROPERTY`s (e.g. future external/VST plugins). Decide whether the first
  consumer (track volume + tw303a params) is fully expressible as properties.

## 6. Phased plan (mapped to the use cases)

### Phase 0 — Foundation (no audio)
`Timeline` + `ControlCurve` data types; stable lane/point IDs; immutable-snapshot
+ double-buffer swap (mirror `SCutSnapshot`); serialize into the existing XML
(lane = child element, points as sorted attributes/rows). Editor stub. Proves the
container, persistence, and the snapshot handoff with zero DSP risk.

### Phase 1 — Sample volume envelope (use case A)
`ControlCurve` on the `SCut`/content window; applied as a **per-sample gain**
multiply in the reader/grain chain. Self-contained: no tempo, no addressing, no
control-rate engine. Best first proof that an evaluated curve reaches the audio
thread correctly and click-free.

### Phase 2 — Parameter automation (use case B, then C)
`ParamRef` + a **control-rate evaluator** that, each block before the audio pull,
evaluates active curves and writes the reflected properties (with smoothing).
B (track `Volume`) first; C is the identical mechanism scoped to the `SLink` in
clip-local time (the curve moves and trims with the clip).

### Phase 3 — Tempo / meter (use case D)
The privileged project-level **tempo map** (monotone samples↔beats) + **meter
map** (bar structure). Built per Decision 5.1. Provides bars/beats display and the
quantize grid. No new lane kind for tempo display — it is special-cased because
everyone else depends on it.

### Phase 4 — MIDI (use case E)
Two sub-deliverables, in order:
- **4a — data:** the `EventStream` lane (note-on/off paired into notes, CC,
  pitch-bend), editor (piano roll), and file import/export. The *easy* half.
- **4b — producer:** an instrument `twComponent` that schedules notes and
  allocates voices on the audio thread, with the tw303a synth as the first
  target. The *larger* half; do not underestimate.

## 7. Relationship to existing code / proposals

- **`03_ACTION_MODEL` / `03a` / `11_ACTION_SCRIPT_TEST_CASES`** — every timeline
  edit is an `SAction`; coalescing (`mergeKey`) covers curve-drawing. This
  proposal is a major *new client* of the action spine, not a competitor to it.
- **`08_PLUGIN_HOSTING`** — Decision 5.2 (the param contract) must be settled
  jointly with plugin hosting; automatable plugin params and the addressing model
  are the same surface.
- **`06_GRAIN_PLAYBACK`** — Phase 1's per-sample gain rides the same reader/grain
  chain `SCut` already owns; envelope evaluation slots in beside grain.
- **`10_RENDER_CACHE`** — automation makes a render *time-varying*, so a cached
  block is only valid for a given automation state; fine invalidation (proposal
  10 Phase 4) must treat automation edits as a dirtying input. Note the
  interaction; do not solve it here.
- **Concurrency** — reuses the `SCutSnapshot` double-buffer and the audio-thread
  rules documented in `docs/AUDIO_IO_ARCHITECTURE.md`; introduces no new model.

## 8. Acceptance (per phase)

- **P0:** a `ControlCurve` round-trips through save/load; a UI edit swaps the
  audio-visible snapshot without a lock on the audio thread.
- **P1:** a sample with a fade-in/out envelope renders the gain shape; no zipper
  noise; the envelope follows the content to every placement.
- **P2:** automating a track's `Volume` audibly follows the curve during playback
  and render; per-clip automation moves/trims with its clip.
- **P3:** bars/beats display and quantize grid match a known tempo/meter; tempo
  edits behave per Decision 5.1.
- **P4a:** a MIDI clip round-trips (import → edit → export). **P4b:** a MIDI clip
  drives the tw303a instrument to audio with correct timing and polyphony.

## 9. Deliberately deferred

- Curve shapes beyond step/linear (bezier/exponential) — add after P1.
- Sample-accurate parameter automation for non-gain params (P2 is control-rate +
  smoothing first).
- External/VST MIDI routing and MIDI hardware I/O.
- Beat-native storage (Decision 5.1 Option B) unless explicitly chosen.
- Automation-aware render-cache invalidation (tracked under proposal 10).
