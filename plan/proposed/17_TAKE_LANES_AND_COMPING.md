# Proposal 17: Take lanes, comping, and edit-locked track groups

> **Status: ALL FOUR PHASES EXECUTED 2026-07-13** (loop recording deferred
> as designed) — see `plan/STATE.md`. Deviations from the design below:
> STakeStack lives in `objects/cut` (not a new `objects/takes` slice) — the
> stack contains cuts and the cut actions dispatch on stacks, so a separate
> slice would be a module cycle. Phase 2 added `SCompositeAction` +
> `place-clip` so `place-recording`'s plan applies and undoes atomically.
> Phase 3 kept rows UNIFORM height (a take lane is just another row), so no
> per-row y-table was needed. Phase 4 puts the broadcast INSIDE the clip
> verbs (`broadcast` attribute + composite fan-out) rather than at the UI
> submission layer, so scripts get group semantics too; the group flag and
> helpers live in `model` (`SObject::editGroup`, `seditgroups.h`) because
> objects/cut and objects/track may not include each other.
>
> **Decisions (2026-07-13, all open questions resolved):**
> 1. Newest take appends at the bottom lane and is auto-activated.
> 2. Single-take stacks collapse back to plain cuts.
> 3. Slip/pitch in an edit group: **synced to the CORRESPONDING take** on
>    the other member tracks (same column, same take index) — never to
>    other takes/lanes of the same track. Use case: fixing drum timing
>    across all mics of the kit.
> 4. Groups are **arbitrary track sets** (à la Pro Tools), not tied to the
>    hierarchy; the folder-track "G" button is an easy UI shortcut that
>    populates a group with the folder + its descendants.
> 5. Loop recording (one take per cycle pass) is deferred to a later phase.

Multi-take recording and comping, as known from Logic ("take folders"),
Studio One ("layers"), Reaper ("takes/lanes"):

- Recording onto a track that already has material stacks a new **take**
  instead of overwriting; a track holds parallel takes with exactly one
  audible at a time.
- **Comping**: split the takes into regions ("columns") and click, per
  column, which take plays. Length/placement edits (split, move, stretch)
  apply to ALL parallel takes; per-take edits (slip, pitch) apply to the
  clicked take only.
- **Group lock**: a folder track can lock itself and its child tracks
  together (multi-mic drum recording); any applicable edit on one member is
  carried out on all members, including take selection.

Terminology note: the word "lane" is already taken in this codebase —
`splacements`/`laneAt` call any path-container (track or mixer) a lane. The
new model object is therefore named **`STakeStack`**; "take lane" is used
only for the UI rows of the expanded track view.

## 0. What the codebase already gives us

| Need | Existing mechanism |
|---|---|
| A clip identity that survives overlaps and shared media | `twTrackMix` clips keyed by `SLink*` (CLIP_MODEL.md) |
| A clip whose component can change identity | `twView` resolves `getComponentFn` lazily |
| Cheap audibility switching | scoped invalidation (`SObject::invalidateRenderPath`, prop. 15) + dropout-free re-freeze during playback (prop. 16) |
| Track nesting for groups | folder tracks (`STrack::isPathContainer`, `SReparentTrackAction`), rendered as indented rows with fold triangles |
| One gesture → N model edits → ONE undo step | `QUndoStack` macros (already used for multi-selection duplicate) |
| Scriptable, undoable edits | `SAction` registry + `.qxa` runner |
| Persistence of new types/attrs | `registerSObjectClass` + tolerant attribute readers |

What is missing: (a) a model object representing a **column of parallel
takes**, (b) per-take audibility that reaches the engine, (c) recording
that goes through an action, (d) an edit-broadcast layer for groups,
(e) the expanded-track UI.

## 1. Data model: `STakeStack` — the column is an object

### Considered and rejected: takes as flat links with lane attributes

The lighter alternative — give `SLink` a `takeLane` int and `takeMuted`
bool, keep all takes as direct track children — needs no new class, but
every length-affecting action (`split-clip`, `move-clip`, `resize-clip`)
must then *discover* the parallel takes by heuristic (window overlap /
equality) and fan out. Once takes have unequal lengths or an edit misses a
sibling, correspondence degrades silently: this is where "linked items"
bugs breed in other DAWs. Rejected in favor of making correspondence
structural.

### Chosen: a container between the link and the cuts (Reaper's item/takes)

```
STrack → SLink(startTime) → STakeStack            (the COLUMN)
                              ├─ SLink → SCut     (take 0: window, slip, grain)
                              ├─ SLink → SCut     (take 1)
                              └─ SLink → SCut     (take 2)   activeTake_ = 2
```

`STakeStack : SObject` (new slice `objects/takes/`):

- `int activeTake_` — index into `childLinks()`; **-1 = none audible**
  ("at most one lane selected"). Signal `activeTakeChanged(int)`.
- `hasDuration() = true`; `getDuration()` = the shared window duration (all
  take cuts are kept at the same timeline duration — enforced by the stack,
  see below).
- Delegation to the active take: `getRootComponent()`,
  `mapTimelineToComponentPos()`, `getPreview()/getCapture()`,
  `getInlineRenderer()` (compact mode draws exactly like today). With
  `activeTake_ == -1` the root component is null → `twTrackMix` mixes
  nothing (same as a missing clip).
- **Window write-through**: `STakeStack::setWindow(duration, stretch)`
  forwards to EVERY take cut (each keeps its own `startOffset_`,
  `pitchCents`, grain params). This single method is where "length ops
  affect all lanes" lives — not scattered over N actions.
- Serialization: one `registerSObjectClass("STakeStack", …)`,
  attribute `activeTake`. Old projects load unchanged (no stacks).

**Plain cuts stay plain.** A stack is created only when a second take
appears (recording over existing material, or explicit "add take"). With
one take there is no stack — zero behavior/UI change for existing
projects, and the compact rendering path is untouched.

### Why the engine does not change at all

To `twTrackMix`, a stack is one clip: `STrack::trackChildWasAdded` already
inserts any child with startTime+duration, keyed by the stack's `SLink*`,
with `getComponentFn` and `mapPosFn` resolving lazily *through the stack to
the active take*. Switching the active take is therefore:

```
stack.setActiveTake(k)  →  invalidateRenderPath()   (prop. 15: only this
                                                     track's path re-freezes)
                        →  next freeze pulls take k's component through the
                           SAME twView (lazy identity change, CLIP_MODEL)
```

During loop playback — the comping workflow — proposal 16 keeps the old
audio playing until the re-frozen pages land. Comping while looping is
exactly the "Ableton-grade" case those two proposals were built for.
No `ClipEntry` change, no per-clip mute in the engine, no new invalidation
paths.

### Take-cut invariants (enforced by `STakeStack`)

1. All take cuts have the same timeline duration (= stack duration).
   Per-take `startOffset_` (slip), `pitchCents`, grain params are free.
2. `activeTake_ ∈ [-1, childCount())`.
3. A stack with one remaining take collapses back to a plain cut link
   (keeps the tree canonical); a stack never has zero takes (delete last
   take = delete the column).

## 2. Actions (all undoable, all `.qxa`-scriptable)

New verbs (slice `objects/takes/`):

| Verb | Attributes | Effect |
|---|---|---|
| `select-take` | `clip` (stack path), `take` = "-1" | set `activeTake_`; inverse restores previous |
| `add-take` | `clip`, `filePath`, `startOffset` = "0" | add a take cut to a stack — wraps a plain cut into a stack on first use |
| `remove-take` | `clip`, `take` | remove one take; collapses per invariant 3 |
| `place-recording` | `trackPath`, `filePath`, `timePos` | the recording-placement verb (see §4) |

Changed verbs — they become **stack-aware, still single-target**:

- `split-clip` on a stack: split the stack into two stacks at `t`; each
  take cut is split with today's `SCut` logic (head keeps window start,
  tail gets `startOffset += t`), `activeTake_` copied to both. Inverse =
  stack-aware unsplit.
- `resize-clip` on a stack: `duration`/`stretch`/`loopLength` go through
  `STakeStack::setWindow` (all takes); `startOffset` goes to the ACTIVE
  take only (slip is per-take by spec). The gesture layer (Alt-drag slip in
  a take lane row) passes the take index explicitly via a new optional
  `take` attribute.
- `move-clip`, `duplicate-clip`: already operate on the link/placement —
  they work on stacks unmodified (duplicate deep-copies the stack).

Pitch stays reachable via `set-property` on the take's `SCut` (as today);
a first-class `set-pitch` verb is out of scope.

## 3. UI: compact and expanded (take-lane) mode

### Compact (default) — no change

The stack's inline renderer delegates to the active take's cut renderer;
the only addition is a small badge ("3", take count) in the clip corner.
`activeTake_ == -1` renders the clip body hatched/dimmed.

### Expanded — per-track toggle

- New checkable button in `SSMVMixerControl` (next to mute/solo/arm, the
  documented extension point) plus a context-menu entry: "Show take lanes".
  State is UI-only (like `collapsed_`: a `QSet<STrack*>` in
  `SStdMixerView`), not serialized into the project.
- `SStdMixerView::rebuildRows()` / `STrackRow` gain a sub-row concept:
  `{…, int takeRow}` where `takeRow == -1` is the composite row (today's
  rendering) and `takeRow == k` renders, for each stack on the track, take
  k only (empty where a stack has fewer takes; plain cuts appear on the
  composite row only). Expanded track height = `(1 + maxTakes) *
  trackHeight_` — the uniform-row-height assumption in `paintEvent`,
  `rowAt`, `getSLinkVisibRect` and control-column placement changes to a
  per-row y-offset table (prefix sums), which is also the prerequisite for
  future per-track heights.
- Take lane rendering: inactive takes dimmed, active take highlighted;
  clicking a take's waveform submits `select-take` (undoable). This is the
  comping gesture: after splitting, each column is its own stack, so
  clicking activates exactly that region — "activate a part of a lane".
- Gestures in take rows: body-click = select take; Alt-drag = slip (that
  take); border/Ctrl gestures operate on the stack (length ops → all
  lanes, per spec). Hit-testing extends `updateLastClickVars` with the
  take-row index.

## 4. Recording: multi-take by default, and finally undoable

Today `SMainWindow::onRecordingCompleted()` places cuts by raw model
mutation (not undoable, not scriptable) and only finds armed tracks among
the root mixer's direct children. Changes:

1. **Armed scan recurses** into folder tracks (fixes a pre-existing gap).
   Arming a group-locked folder track arms all its descendants ("select
   tracks or track groups for recording").
2. Placement becomes a `place-recording` action per armed track (wrapped
   in one undo macro for the whole recording pass): given the WAV path and
   start time, it
   - finds a take stack or plain cut overlapping `[start, start+dur)` on
     the target track → adds the recording as a NEW take (wrapping a plain
     cut into a stack first) and makes it active;
   - places a plain cut if the range is empty (today's behavior).
   Overlap-with-partial-cover is resolved by splitting the recording at
   the boundaries of existing columns (one new take per touched column,
   plain cuts for uncovered gaps) — keeps invariant 1 without disturbing
   neighbors.
3. Because it is an action taking a file path, the whole multi-take flow
   becomes headless-testable with fixture WAVs — no audio input device
   needed in qxa.

## 5. Track groups: the edit lock

### Model (decision 4: arbitrary group sets)

Group membership is NOT tied to the track hierarchy. `STrack` gains
`int editGroup_` (+ signal, serialized as an attribute, `0` = ungrouped);
tracks sharing a nonzero id form one edit group. A small registry view
(`SEditGroups`, computed over the tree — no separate persisted object)
answers `membersOf(groupId)` and `groupOf(track)`.

UI: the "G" toggle in `SSMVMixerControl` is the easy shortcut — on a
folder track it assigns a fresh group id to the folder and all descendant
tracks (toggle off clears them); on a leaf track inside a group it leaves/
rejoins that group. Arbitrary membership editing beyond the shortcut goes
through a `set-edit-group` action (`trackPath`, `group`), so any set of
tracks can be grouped regardless of nesting. Grouped tracks' headers are
tinted per group.

### Broadcast at the submission layer, not inside actions

Actions stay single-target (clean inverses). The fan-out lives where the
multi-selection duplicate precedent already is — at gesture finalization /
action submission:

- New helper slice `main/groups/`: `SGroupBroadcast::expandTargets(SLink*
  anchor, Verb) → QList<SLink*>`. If the anchor's track is inside an
  edit-locked family, it returns the corresponding link on every other
  member track; otherwise just the anchor.
- **Correspondence is positional**: the link on the sibling track with the
  same `(startTime, duration)` window. This is sound *because the lock
  keeps it sound*: every length/placement edit while locked is broadcast,
  so windows never diverge; a multi-mic recording pass creates identical
  columns on all members (§4). If a sibling has no matching link, that
  target is skipped ("as applicable") with a status-bar note.
- The submitting site wraps the N cloned actions (path rewritten per
  target) in one `QUndoStack` macro — one Ctrl-Z undoes the group edit.

### What broadcasts

| Verb | Lanes (within a stack) | Edit group (across tracks) |
|---|---|---|
| split / move / resize(duration, stretch, loop) / duplicate / delete | all takes (structural, via stack) | yes (broadcast) |
| `select-take` | n/a (it IS the selection) | yes — same take index on each member (drum-kit comping) |
| slip (`startOffset`), pitch, grain params | active/clicked take only | **yes — to the CORRESPONDING take** (same column + take index) on each member, never to other takes (decision 3; drum-timing fix) |
| track mute/solo/volume/plugins | n/a | no — the lock is an EDIT lock, not a mix link |

## 6. What can go wrong (and the guards)

- **Path-index churn**: clip paths are child indices; wrapping a cut into
  a stack inserts a tree level. All wrapping/collapsing goes through the
  two helpers in `objects/takes/` that fix up in-flight selection, and
  actions resolve paths at apply time (as today).
- **`durationChanged` fan-in**: STrack resolves duration changes by
  scanning `childLinks()` for the sending SObject (CLIP_MODEL warning).
  The stack forwards its cuts' `durationChanged` as its own, so the
  existing STrack slot keeps working unmodified.
- **Group lock with diverged tracks** (locked after independent editing):
  positional correspondence finds nothing → edits apply to the anchor only,
  visibly noted. An optional "align group" helper can be added later.
- **Contracts**: CLIP_MODEL.md gains a fourth layer description (stack);
  ACTIONS.md rows for the new verbs; new `objects/takes/CONTRACT.md`.

## 7. Phasing (each phase independently shippable & tested)

1. **Model + audibility**: `STakeStack`, invariants, serialization,
   `select-take`/`add-take`/`remove-take`, stack-aware split/resize.
   qxa: build a 2-take stack from fixture WAVs, `select-take`, render,
   assert per-second RMS flips between the takes; `verify-undo`.
2. **Recording through actions**: `place-recording`, recursive armed scan,
   undo macro per pass. qxa: two `place-recording` passes → stack with 2
   takes, newest active.
3. **Expanded UI**: per-row y-table, take rows, click-to-comp, slip
   gesture in lanes. Screenshot qxa cases.
4. **Edit groups**: `editGroup_` id, `set-edit-group` action, "G" shortcut
   button, `SGroupBroadcast`, macro submission. qxa: 2-track group,
   `split-clip` on one → assert both split; `select-take` on one → both
   flip (render + RMS); `resize-clip` slip on one → corresponding take on
   the other slips too.

## 8. Open questions

All resolved 2026-07-13 — see the decision block at the top. Loop
recording (one take per cycle pass) remains the designated phase 5;
the stack model supports it, the recording session cannot yet segment
passes.
