# Concept: Track Grouping & Live Region Assets

Design only. Two requested features that, on inspection, share one core idea and
should be built on a single foundation.

## TL;DR

Both features are *composition of sub-arrangements*:

- **(a) Track groups** = a track that, besides its own clips, sums the output of
  child tracks and runs the sum through its own processing.
- **(b) Live region assets** = a marked region becomes a shareable sub-arrangement
  object placed elsewhere via `SLink`/`SCut`; editing it changes every instance.

The object model already has almost everything needed:

- `twTrackMix::calcOutputTo()` already sums `lk->getRootComponent().calcOutputTo()`
  over **all** children of a track — it does not care whether a child is an
  `SCut`, an `SExternFile`, or another `STrack`. So nesting is *already* how the
  DSP works.
- `SObject`s are reference-counted and shared via `SLink`; the render path is a
  **live pull** every buffer. So "edit once, hear everywhere" is automatic for
  anything referenced by more than one `SLink` — no rendering-to-file needed.
- `SCut` is already a *windowed view* (`startOffset_`, `cutDuration_`,
  `loopStart_`) into a shared `SObject` — exactly "use the whole thing, or a
  part".

The one real architectural change required is **making a track's processing
intrinsic to its output** (see §1). Everything else is new objects, UI, and a
range-selection model layered on the existing primitives.

---

## 0. The one prerequisite refactor: intrinsic track processing

**Today:** a track's volume/mute/solo is applied by its *parent mixer*
(`SStdMixer::reconnectTracksToMixer` → `twMixer::setInputLevel`, and the
mute/solo routing just added). `twTrackMix` itself sums children at unity gain;
`STrack::getRootComponent()` returns the raw, pre-gain sum.

**Problem for nesting:** if track B becomes a child of track A, A's `twTrackMix`
sums `B.getRootComponent()` — which is B's *pre-gain* output. B's own
volume/mute/solo would be lost, because nobody applies them (only a top-level
mixer input does today).

**Change:** push gain/mute/solo (and later pan) **into the track strip**, so that
`STrack::getRootComponent()` already reflects the track's own processing. The
strip becomes: `children → twTrackMix (sum) → gain/mute stage → busses`. Then:

- A track is a **self-contained source**: wherever it is summed (top mixer *or* a
  parent track), its processing is already baked in.
- Summing becomes uniform everywhere. `SStdMixer` degenerates into "sum your
  children" — the master is just another group.
- Mute/solo become intrinsic too. (Solo stays slightly special — it is a
  *project-global* relation: "any soloed track silences the others" — so it is
  still resolved at the enclosing container, not purely locally. See §1.3.)

This refactor touches the mute/solo work just landed; it is the foundation for
(a) and makes (b)'s multi-track assets fall out for free. Recommend doing it
first as its own phase.

---

## 1. Feature (a): Track groups (folder tracks)

### 1.1 Model

Tracks form a **tree** instead of a flat list. A track's children list already
holds `SLink`s; allow some of those links to point at child `STrack`s (start
time 0, i.e. time-aligned with the parent) in addition to clip links.

```
SStdMixer
 └─ STrack "Drums"            (parent / folder)
     ├─ SLink → SCut (a clip on the Drums track itself)
     ├─ SLink → STrack "Kick"     (child track, startTime 0)
     └─ SLink → STrack "Snare"    (child track, startTime 0)
```

- **DSP:** no new component needed. `twTrackMix` already sums every child's
  `getRootComponent()`. With §0 in place, each child track contributes its own
  processed output; the parent's strip then applies the parent's gain to the
  whole sum. Recursion is automatic (a child track may itself be a folder).
- **Distinguishing clips from child tracks:** by the linked object's type
  (`dynamic_cast<STrack*>`), or a small role flag on the link. The model needs no
  new container type — a "group" is just a track that happens to have track
  children.

### 1.2 UI (arranger)

This is the bulk of the work. Today `SMVActualView` iterates `getTrackAt(i)` over
a flat list. It must instead **walk the track tree depth-first**, drawing each
track as its own lane, indented by depth, with a fold/▾ triangle on parents.
`SSMVMixerControl` gets an indent + a collapse toggle. Track index ↔ lane
mapping (used all over the view for hit-testing, drag, `lastClickTrackIdx_`)
becomes a flattened-tree traversal rather than `children().at(i)`.

### 1.3 Solo with groups

Solo's "silence the others" is global. With intrinsic processing, the cleanest
rule: a track is audible iff `!muted && (!anySoloedInProject || isSoloedOrHasSoloedDescendant)`.
Soloing a folder should keep its children audible. Compute `anyTrackSoloed()`
over the whole tree (it currently scans the mixer's direct children only).

### 1.4 Actions / serialization

- Reparent/group via a new `SReparentTrackAction(track, newParent, index)`
  (undoable; cf. proposal 03). "Group selected tracks" = create a folder track +
  reparent.
- Serialization needs **no format change**: nesting is just `SLink` children, and
  the loader already rebuilds arbitrary `SObject`/`SLink` trees by id. (A track
  child of a track already round-trips structurally.)

### 1.5 Risks

- The view's flat-index assumptions are pervasive — the tree-flattening is the
  real cost, not the engine.
- Mixed children (clips *and* child tracks on the same lane) — decide whether a
  folder track may also host its own clips (Reaper: yes). The DSP allows it; the
  UI must draw both.

---

## 2. Feature (b): Live region assets

### 2.1 What it is

A marked time region over one or more tracks becomes a **named asset** in the
resource list that behaves like a sample: drop it anywhere via `SLink`/`SCut`,
whole or partial. It is **not** rendered to a file — it is a live reference, so
editing it re-plays everywhere. Recursive.

### 2.2 It is already 90% the existing model

- "A reusable thing in the resource list" = an `SObject` registered like an
  `SExternFile` is today (`SProject::externFileDict_`, shown in
  `SExternFileList`).
- "Placed anywhere, whole or in part" = `SLink` (placement) + `SCut` (windowed
  view: `startOffset`/`cutDuration`/`loopStart`).
- "Edit once → changes everywhere" = `SObject` sharing + the **live pull** render.
  Multiple `SLink`/`SCut` referencing one `SObject` already all reflect its
  current state every buffer. No file, no baking.
- "Recursive" = the asset is an `SObject`; it can contain `SCut`s of other
  assets, and a region used to build an asset may itself contain instances.

So a single-track region asset is essentially **an `SCut` (or the underlying
`SObject`) promoted into the resource list**. A multi-track region asset is **a
group** (§1) promoted into the resource list — which is exactly why (a) is the
foundation for (b).

### 2.3 The asset object

Introduce `SAsset` (name + an `SObject` body) or, more simply, reuse the group
container from §1 as the body and add an *asset registry* entry (id + display
name + "is a resource" flag). A multi-track asset's body is a group whose child
tracks are the selected tracks' slices; a single-track asset's body is a track or
even a bare `SCut`.

Crucially the asset is a normal shared `SObject`: instances are `SLink`/`SCut`
into it, reference-counted as usual. The **resource list holds one reference** so
the asset survives with zero arrangement instances (otherwise refcount→0 →
`deleteLater`).

### 2.4 Extraction algorithm (mark region → asset)

Given a region `[t0, t1)` and a set of tracks `T`:

1. Create the asset body (a group with one child track per track in `T`).
2. For each track in `T`, copy/move the clips intersecting `[t0,t1)` into the
   corresponding asset child track, **time-shifted by −t0** (so the asset starts
   at 0), splitting clips at the boundaries (the `ensureSCut`/split machinery in
   `SStdMixerView::ctSplitSample` already exists).
3. Register the body in the resource list with a generated name ("Drum loop 1").
4. **Replace the original region in place with an instance**: drop an `SCut` →
   asset (windowed to the region length) at `t0` on a track. Now the original
   location is itself an instance, so editing through it edits the shared master
   → all instances change. This is what makes "move one bit in the original,
   it plays differently everywhere" true.

"Edit through an instance" works because an `SCut` is a *window* onto the shared
body; opening it for editing edits the body, not a copy.

### 2.5 Share vs. copy (key decision)

For "edit the original ⇒ all instances change", the asset body must be the
**single source of truth** and the original spot must become an *instance* of it
(§2.4 step 4). If instead extraction left the original clips loose and only
*copied* them into the asset, the two would diverge — contradicting the spec. So:
**extract-and-replace**, not copy-and-keep.

### 2.6 Region selection model (the "yet to be defined region selector")

Today's selection (`SApplication::selectionList_`, `currentSelectedSLink_`) is
*object-based*. Assets need a **range selection**: a time interval plus a set of
tracks. Propose `SRangeSelection { offset_t start, end; QSet<STrack*> tracks; }`,
ideally living in `SProject` as first-class state (per proposal 03's
"selection as state") so it participates in undo/scripting. UI: rubber-band drag
across the time ruler / track lanes.

### 2.7 Recursion & cycles (must-have)

Assets reference assets → a directed graph that **must stay acyclic** (a cycle
would make `calcOutputTo` recurse forever). Add an acyclicity check before
allowing a placement: "does inserting asset X here create a path X → … → X?".
Reference counting already tracks edges; add a DFS reachability guard in the
insert/`setParent` path (and in `SReparentTrackAction`).

### 2.8 Duration / length propagation

`SCut` has its own `cutDuration_` independent of the content's duration — good
for "use a part". When the master grows/shrinks, an instance windowed to the old
length keeps its window (stable), while an instance meant to track the whole
asset would want to follow `content.getDuration()`. Decide per-instance: a
"follow length" flag, or simply re-derive from the content when `cutDuration_`
is unset. `durationChanged` already propagates from content; instances can
listen.

### 2.9 Serialization

The flat-with-ids + `objectId` reference scheme the loader already uses handles
shared assets directly (an `SObject` referenced by several `SLink`s serializes
once; links reference it by id). Needed additions:

- Mark asset bodies as **resources** (so they load into the resource list even
  when momentarily unreferenced) and store their **display name**.
- Ensure the resource-list reference is (re)established on load so assets aren't
  garbage-collected.

This is a small, additive format change (a `<assets>`/resource section, or an
`isAsset`/`name` attribute on the body element).

### 2.10 Threading

The live-pull render means an instance reads the master on the audio thread while
the user may edit it on the UI thread — the same hazard already documented for
`SCut`/`twWavInput` (see THREAD_SAFETY notes). Structural edits (add/remove/split
clips) must be sequenced safely w.r.t. the audio callback — a natural fit for the
`SAction` engine-thread-drain model (proposal 03) once it goes async.

---

## 3. How the two features relate

```
intrinsic track strip (§0)
        │
        ├──> (a) groups: a track summing child tracks + own clips
        │
        └──> (b) assets: a *group* (or track/cut) promoted into the
                 resource list and referenced by SLink/SCut instances
```

(b) is (a) + "register as a shared resource" + "range selection" +
"extract-and-replace" + "cycle guard". Building (a) first makes (b) mostly
assembly of existing parts.

---

## 4. Suggested phasing

1. **Intrinsic track processing** (§0). Move gain/mute/solo into the track strip;
   reduce `SStdMixer` to a summer. Re-verify audio + the mute/solo just added.
2. **Track tree model + reparent action** (§1.1, §1.4). Engine/data only;
   round-trip through save/load.
3. **Grouped arranger UI** (§1.2): tree-flattened lanes, indentation, fold,
   group/ungroup. The big UI lift.
4. **Range selection model + selector UI** (§2.6).
5. **Asset object + resource-list integration + extract-and-replace** (§2.3–2.5,
   §2.9), with the **cycle guard** (§2.7) in place from day one.
6. **Length-follow + edit-through-instance polish** (§2.8), and async-safe edits
   (§2.10).

## 5. Open questions

- May a folder track also carry its own clips (Reaper-style), or is it
  summing-only? (DSP allows either.)
- Does soloing a folder solo its descendants implicitly? (Recommended: yes.)
- Asset instances: default to "follow master length" or "fixed window"?
- Where does the asset open for editing — in place (the instance expands) or a
  dedicated editor view? (Affects "edit the original" UX.)
- Resource lifetime/GC: explicit "delete unused asset", or keep until project
  close?
- Should `SStdMixer` be retired in favour of "the master is just the root group",
  or kept as the master container?
```
