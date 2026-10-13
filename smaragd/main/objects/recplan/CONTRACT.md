# app/objects/recplan — the record planner

**HOW A RECORDING PASS LANDS ON A LANE THAT ALREADY HAS MATERIAL.** One
`srecplan::Planner` per record mode (plan 53 §5.1). `place-recording` and
`place-midi-recording` are THIN VERBS: each reads its lane once into a
`LaneSnapshot`, picks the planner for the mode it was handed, and applies
whatever the planner appended to one composite. The verb decides nothing.

## Why this is its own module

It used to live in `objects/cut`, beside the verbs it emits — and that put it
out of the EVENT side's reach for good. `tools/check_layering.py` gives
`objects/midi` the edges `{actions, model, persistence}`, deliberately:
proposal 37 §3.5 says *"objects/midi sits at the RANK of objects/cut … it is a
second window/content pair, not a layer above one, and it must stay
independent of the audio window."*

So `place-midi-recording` could name neither `srecplan::snapshot()` nor
`merge-columns` — the verb plan 53's mode A is built on — and P3 could not be
the wiring it was scoped as (QBX-181, QBX-188).

This module sits at the RANK of both. `objects/cut` and `objects/midi` each
depend on it; **neither depends on the other, and no existing edge was
broken.** One copy of the union rule, reachable from both kinds.

Its own dependencies are the floor — `{actions, model, persistence}` in the app
layer and nothing beyond `_ENG_BASE` in the engine layer. It names no concrete
clip, no concrete column and no concrete verb.

## The four rules every planner is held to

1. **A PLANNER EMITS EXISTING VERBS and never mutates the model.** Its undo is
   the composite's inverse chain, and it is testable by the verbs it emits
   rather than only through the model it leaves behind.
2. **`snapshot()` IS THE ONE READER OF LANE STATE**, and it CLASSIFIES without
   mutating. No planner may look at the lane again: a planner that read the
   live tree halfway through its own plan would be reading a tree its own
   earlier verbs have not been applied to yet.
3. **INDEX BOOKKEEPING IS THE PLANNER'S, IN APPLY ORDER** (plan 53 G10). Lane
   child indices are INSERTION order, not time order, and the verbs a planner
   emits change them: a placement and a split APPEND, adding a take preserves
   its operand's index, and `merge-columns` keeps its FIRST operand's index and
   REMOVES the others. So a plan that needs a path after it has emitted
   anything structural asks `ChildList`, never the snapshot.
4. **IT NAMES NOTHING CONCRETE.** Every verb goes through
   `srecplan::PassVerbs`, supplied by the caller's own module; every fact about
   a lane child comes from `SObject`'s kind-agnostic accessors.

## `PassVerbs` — the seam

`objects/cut` implements it as `SAudioPassVerbs` (`place-clip`, `add-take`,
`split-clip`, `remove-sample`, `merge-columns`); the event side implements it
in `objects/midi`. It follows the precedent
`SClipWindow::registerTakeColumnFactory` already set for this exact layering
problem — `smidirecordactions.cpp` wraps a clip into a take column through the
model's registered factory precisely so that *"`objects/midi` never names
STakeStack"*.

**EVERY POSITION ACROSS THE SEAM IS A TIMELINE POSITION, and the SOURCE
arithmetic belongs to the implementation.** The planner knows where the pass's
material has to start on the timeline; only the audio side knows that this
means `srcOffset + (takeStart − a)` frames into a WAV, and only the event side
knows it means a tick rebased through the clip's own window. A planner doing
that arithmetic itself would be doing it in one domain for both.

`Span::filePath` and `Span::srcOffset` are therefore the AUDIO payload, read
only by the audio implementation. An event implementation carries its own
events and ticks, handed to it by its verb for that one pass.

**A plan with no verbs REFUSES** rather than emitting nothing: an empty
composite applies as SUCCESS, which is exactly how a recording goes missing
silently (the defect `place_recording_over_earlier_clip` gates).

## What `snapshot()` may ask

Only `SObject`'s kind-agnostic accessors, which is what lets one classifier
serve both kinds:

| question | accessor |
|---|---|
| is it a column? | `windowTakeCount() > 0` |
| DIRECT or WRAPPED? | whether the link's own object is a window (`SClipWindow::of`) |
| how many takes, which is active? | `windowTakeCount()`, `activeWindowTakeIndex()` |
| does it carry a comp map? | `hasCompMap()` |
| its mix parameters | `SClipWindow::parametersOf`, `automationLane`, `clipFade` |

`SCut` FORWARDS all four take accessors to its content, which is what makes the
WRAPPED shape answer for the column it windows. `hasCompMap()` was added for
this module (QBX-188); `Child` carries a BOOLEAN rather than the `twCompMap` it
used to, because nothing ever read the map itself — only the
`Column` / `MappedColumn` distinction, which no planner branches on. That is
what costs this module its last engine dependency.

## Known debt

- **The modes' MIDI meanings are not implemented here yet** (QBX-181 / plan 53
  P3). This module is the move that makes them possible; the event
  `PassVerbs` and `place-midi-recording`'s switch to the planner split are
  P3's.
