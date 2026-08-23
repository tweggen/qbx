# Proposal 42 — A take column has ONE shape, and one placement

Status: **OPEN**. Written 2026-08-23 after an architecture review commissioned
by the repo owner, whose hypothesis was that a cluster of defects in
green-field code points at an architecture problem rather than at bad luck.
It does. This proposal records the finding and the repair.

## 1. The finding

A take column reaches a track in two structural spellings:

```
DIRECT    SLink -> STakeStack                    what add-take builds
WRAPPED   SLink -> SCut/SMidiCut -> STakeStack   nothing designs; three things produce
```

**43 sites in `main/` resolve a take column from a placement. 10 handle both
shapes. 31 are DIRECT-only.** By layer: 14 of 18 in actions, 11 of 18 in UI,
2 of 3 in engine, 4 of 4 in testkit.

Ten defects were found and (mostly) fixed one at a time between 2026-08-21 and
2026-08-23 — PRs #133, #134, #135, #138 plus two still open. Every one of them
is a consumer that knew one spelling. They were not ten mistakes; they were ten
of the thirty-one, surfacing in the order a user happened to touch them.

**Two invariants exist in prose and nowhere in code:**

1. *An `STakeStack` is only ever the direct object of a placement link.*
2. *A take column belongs to exactly one placement.*

Neither is checkable. A `dynamic_cast<STakeStack*>( &link->getSObject() )` that
forgets the wrapped shape is indistinguishable, to every compiler and every
test, from one that handles it. That is a defect GENERATOR, and the rate of
defects is proportional to the number of consumers.

## 2. Where the wrapped shape comes from — three producers, all measured

| producer | what it does |
|---|---|
| `SStdMixerView::ensureSCut` (7 gesture call sites) | any border / trim / stretch / loop drag wraps the placement's object in a new `SCut` **during a mouse-move**, and deletes the old link |
| `SDuplicateClipAction` (`duplicate-clip`, Ctrl-drag) | `SClipWindow::of()` is null for a stack, so it `wrapContent`s the copy over the **SAME** stack |
| `SStdMixerView::ctAddLink` | a second **DIRECT** link to the same stack — shared comping with no wrapper at all |

No design document describes the wrapped shape and no verb creates it on
purpose. `main/objects/cut/CONTRACT.md` currently describes it as "what a
SHARED or PLACED column is"; that sentence was written while fixing #138 and
retroactively legitimises an artifact.

**Forensics on the reporting user's project** (`test5.qxp`, track "Solo"): the
stack is 768000 frames, the wrapper is `srcStart='96000' cutDuration='672000'`,
and 768000 - 96000 = 672000 exactly — the signature of one `ensureSCut` wrap
followed by a single left-edge drag.

## 3. What `ensureSCut` costs beyond takes — measured, not inferred

It tests `qstrcmp( metaObject()->className(), "SCut" )` and wraps anything else.

* **A MIDI clip becomes an AUDIO clip pinned to TIME.** `SLink::timebase` is
  serialized only when it differs from `defaultTimebaseFor( content )`, and
  that default flips Beats -> Time with the class. Measured: `startTicks='0'`
  present before a right-border drag, **absent after**, the object now an
  `SCut`. Proposal 37's central invariant, broken by a mouse gesture.
* **The dragged clip jumps to the END of the lane's child list.** `setParent`
  appends and nothing restores the index — unlike `stakes::wrapCutLinkIntoStack`
  and `collapseSingleTakeStack`, which both call `moveChildToIndex` and say why
  ("undo determinism"). Measured: `[SMidiCut@0, SCut@192000]` becomes
  `[SCut@192000, SCut@0]`. Clip paths are `(track, childIndex)`, so **every path
  already recorded in the undo stack now addresses a different clip.**
* **It leaves a dangling `durationChanged` connection.** The two sibling
  link-swapping helpers each `QObject::disconnect` the old object first and
  document why; this one does not.
* **`add-take` on a wrapped column builds a column INSIDE a column.** Measured:
  the new take becomes a sibling of the entire old column.

## 4. The repair, in milestones

**M1 — close the producers.** `ensureSCut` never converts a placement that is
already an `SClipWindow` or a take column; the border gestures edit a column
through the stack's own write-through (`setDurationAll` / `applyWindowAll` plus
the active take's anchor), which is what `resize-clip`'s direct branch already
does. `duplicate-clip` DEEP-COPIES a column. `ctAddLink` gets an explicit
decision. After M1 no new wrapped column can be created.

**M2 — make the generic seam TOTAL.** `SCut` / `SMidiCut` forward
`windowTakeAt` / `windowTakeCount` / `activeWindowTakeIndex` /
`insertWindowTake` / `removeWindowTake` / `setActiveWindowTake` one level into
a column content. This repairs, in ONE place, every consumer that already goes
through the seam — the MIDI event verbs, the event editor, the virtual
keyboard, the clip-properties panel, the automation owner resolve and four
testkit asserts — and lets `stakes::columnOfLink` retire into `SObject`.

**M3 — the verbs that must address the COLUMN, not the wrapper.**
`split-clip` (which today leaves both halves sharing one `activeTake_`, making
per-region comping impossible on the wrapped shape), `add-take`, `remove-take`,
and `resize-clip`'s left-edge anchor (which moves the extent for every take but
the anchor for the ACTIVE take only, desyncing the takes from each other).
Each needs an explicit decision for a genuinely SHARED column.

**M4 — migrate existing files.** An identity wrapper collapses to DIRECT
(`collapseSingleTakeStack`'s inverse). A NON-identity wrapper — which is what
the reporting user's project carries, so this half is not optional — has its
window COMPOSED INTO each take. Counted and logged either way.

**M5 — the capability gap, separately.** The use-case review mapped ten real
comping workflows onto the model: four are impossible BY INVARIANT (per-take
trim, takes of different lengths, a take offset within its column, two clips on
one take lane) and **no clip fade or crossfade exists anywhere in the model**,
so a comp join can only be a hard sum. These are ADDITIVE features and a
separate proposal; they are recorded here so "the design is fine" does not hide
them.

## 5. The gate this subsystem never had

A fixture carrying the WRAPPED shape, exercised by **every** take verb, plus an
assertion that **no verb and no gesture can produce a wrapped column**. The
second half is what would have caught all ten defects at once, and it is the
only thing that keeps invariant 1 true once M1 makes it true.
