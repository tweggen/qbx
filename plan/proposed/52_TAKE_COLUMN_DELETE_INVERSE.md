# Proposal 52 — a real inverse for deleting a take column

**Ticket:** QBX-146, step (a). **Status:** proposed.
**Written** 2026-10-11; **rewritten the same day** after review returned RED on
its central premise. §9 records what changed and why.

Source locations are given by function or symbol name, never by line number.

Step (b) (#235) stopped the data loss by **refusing** a delete it could not
undo. That is a blocked operation, not a fixed one. This is what lifts the
refusal.

## 1. PIN the content; do not copy it

**The first draft of this plan had it backwards.** It reasoned: `SLink::~SLink()`
ends with `object_.removeRef()`, so content is refcounted, so a column built
for one clip dies with its clip, so *"the inverse cannot hold a reference. It
has to hold a copy."*

The mechanism is right and the conclusion is wrong. An `addRef()` is exactly
how you hold a reference across the delete, and **this codebase already does
it, for this purpose**. `SRemoveTrackAction::apply`:

```cpp
// Pin the track on THIS action so it (and its subtree) survives the removal;
// the undo command reuses this object, so the pin persists across redo.
track->addRef();
heldTrack_ = track;
holdsRef_ = true;
```

with `dropStalePin()` in its destructor, and `SRestoreTrackAction::apply`
re-linking `owner_->heldTrack()` and then calling `owner_->releaseHeld()`. The
same pattern appears in `SProject::registerAsset` (`body->addRef(); // pin: the
asset survives with zero placements`), and `~SProject`'s survivor pass names it
outright: *"something outside the project still references them (an undo-stack
action's pin is the usual case)"*. `SMainWindow::closeProject()` releases pins
before the project dies, naming `SRemoveTrackAction::heldTrack_`.

So the design is: **the forward action pins the clip's own object; the inverse
borrows it, re-links it, and releases the pin.**

A pin preserves every piece of state **by identity**, which is the decisive
argument for it over a copy — see §2 for the list a copy would have dropped.

## 2. What a COPY would have lost, and why that killed the clone design

`stakes::cloneColumn` → `SClipWindow::cloneWindowOverContent` copies the
window (`srcStart`, duration, `loopLength`, stretch), grain params, automation
(`copyAutomationFrom`), `setVolume` and `setPan`. Measured against what the
model actually carries, a clone-based inverse would silently drop:

| Lost by a clone | Where it lives |
|---|---|
| **the comp map** | `STakeStack::compMap_`; `cloneColumn` never calls `setCompMap` |
| **per-take clip fade** | `SCut::fade_` (`twClipFade`), set by `set-clip-fade clip= take=`, audible through `STrack::refreshClipGainCurves` |
| **per-take name** | `set-clip-name take=` sets `win->asObject().setSName()` |
| **MIDI take volume and pan** | `SMidiCut::cloneWindowOverContent` copies ticks, transpose, velocity scale, channel override, automation — but *not* volume/pan, unlike `SCut` |
| **the wrapper's own state** on a wrapped column | `refreshClipGainCurves` reads `SClipWindow::parametersOf()` — the wrapper: its automation, volume, fade, pan |
| **`SLink` state** | `timebase_` / `startTicks_` (the beats authority for event content) and `eventChannelOverride_`, both stored on the link by design |
| **column attributes** | `STakeStack` serialises `muted/volume/pan/colorIndex/editGroup/name`; a fresh `new STakeStack(project)` has none |

Every one is free under the pin. That asymmetry, not elegance, is the reason
for the design.

It also follows that `cloneColumn` should **not** be cited as "proven code" for
faithfulness. It is proven to work for `duplicate-clip`, and proven to drop the
comp map and fades on every caller.

## 3. The pin design, concretely

`SRemoveSampleAction::apply`, for a clip it can pin:

1. resolve the column through `stakes::columnOfLink` — never a bare
   `dynamic_cast`; it handles both shapes;
2. `addRef()` the **link's own object** — the `STakeStack` on the direct shape,
   the wrapper `SCut` on the wrapped one (whose content link holds the stack),
   so the wrapper is preserved too;
3. **`QObject::disconnect( obj, nullptr, lane, nullptr )`** before detaching.
   `STrack::trackChildWasRemoved` does *not* undo the object-level
   `durationChanged` connection that `trackChildWasAdded` made, and those
   connects are not `Qt::UniqueConnection` — so re-linking without this
   doubles the signal. `SRemoveTrackAction` and `stakes::wrapCutLinkIntoStack`
   both already do it;
4. delete the link, keep the pin, and return the inverse holding a
   back-pointer to this action plus `restoreAtIndex` (QBX-149: an inverse owes
   child order back).

The inverse re-links the held object at `restoreAtIndex` and releases the pin.
The forward **re-pins on every apply**, so the pair survives
undo → redo → undo.

### Why "clone once and hold it" could not have worked at all

`SActionUndoCommand::undo()` submits the **same** `inverse_` object every time
— *"The inverse action is reusable… Don't null out the pointer"* — and
`SActionHistory::submit(…, skipHistory=true)` deletes whatever inverse the
forward returns. So: undo (clone linked in) → redo (link deleted, clone's
refcount hits 0 and it is collected) → undo again dereferences a destroyed
clone. The first draft's "delete the clone if the inverse is destroyed without
applying" was the wrong lifecycle model entirely.

### Two consequences to state rather than discover

- **A pending pinned object is written on save.** `SProject::serialize`
  iterates `children()` and writes every one. This is already true of a
  removed track sitting on the undo stack, so it is an accepted property of
  the existing design, not a new defect — but the plan says so rather than
  claiming inertness, which the first draft wrongly did.
- **The forward stays serialisable.** Only the inverse is live-only, exactly as
  `remove-track` is serialisable while `restore-track` is not.

## 4. Recordings: the refusal STAYS, and for ownership, not cost

`SRecordingContent` is the growing clip that exists only during capture. The
first draft guessed its PCM might be expensive to copy. That was never the
issue: it is a `shared_ptr` view.

The real reason is ownership. `SAudioRecorder::Armed` holds

```cpp
SCut  *cut  = nullptr;   // the growing clip (borrowed)
SLink *link = nullptr;   // its placement  (borrowed)
```

touches them on every tick, and `delete a.link`s them at stop. A
`remove-sample` on a live recording is a use-after-free **in the recorder**,
before any undo question arises. There is precedent for refusing it outright:
`SExtractArrangementAction` refuses a live recording because "the take is still
growing under it".

So no measuring case is needed, and the refusal from (b) is retained for this
shape with a message naming the real reason — a live recording belongs to the
recorder until stop.

**Not confirmed:** whether the GUI already blocks a delete during recording.
`SStdMixerView`'s remove path does not appear to check `isLiveRecording()`. If
it does not, that is a separate defect worth its own ticket.

## 5. The wrapped shape survives a load, so the inverse owes it in full

**The first draft claimed a wrapped column never survives a project load.**
Wrong. `stakes::normalizeColumns` folds a wrapper into its takes only when
`foldIsExact`, which **refuses** when the wrapper loops, is stretched, is
panned or carries automation, or when a take loops — logging
`take column left WRAPPED`. `smaragd/CMakeLists.txt` states the consequence
plainly: a wrapper the migration refuses to fold *"survives a load in the
field"*.

Of the committed fixtures, `takestack_wrap_wrapperpan.qxp` (panned wrapper) and
`takestack_wrap_looping.qxp` (looping wrapper) therefore load wrapped **with no
knob at all**; `clip_pan_wrapped_take.qxa` says so in as many words. Only the
exact-fold fixtures need `SMARAGD_TAKE_MIGRATE=off`, and that is set in
`smaragd/CMakeLists.txt`'s `set_tests_properties` for three cases — **not** in
any `.qxa`, and **not** for `take_column_migrate`, which asserts
`shape="direct"` and so needs migration on. The first draft had both details
wrong, having inferred them from one hand-run probe.

The shape is also reachable at runtime: `split-clip` and `duplicate-clip` both
produce a wrapper over a column. So the inverse owes the wrapper's full state —
which, pinning the wrapper object, it gets for nothing.

## 6. Raised, not fixed here

- **`split-clip` drops the tail's comp map, in both branches.** The direct
  branch builds a second stack without `setCompMap`; the wrapped branch goes
  through `cloneColumn`, which never copies it. `unsplit-clip` cannot bring it
  back. The ticket's words were "split/delete operations" — so a comped column
  that is split has already lost the tail's comp **before any delete**. This is
  a stronger instance of the same class than the duplicate question below, and
  it needs its own ticket.
- **`remove-take` has the identical null-inverse hole:**
  `SAction *inverse = filePath.isEmpty() ? nullptr : new SAddTakeAction(...)`.
  A non-file-backed take (a MIDI take) is removed with `{true, nullptr}` —
  exactly QBX-146's silent-permanent pattern, one level down. Out of scope
  here; not out of mind.
- **`duplicate-clip` on a wrapped column** appears to produce a second wrapper
  over the **same** `STakeStack` rather than a copy, contrary to the comment on
  the function that does it. Could not confirm the user-visible consequence.
- **`duplicate-clip` drops the comp map**, and nothing pins whether it should.

## 7. The gate

The case must fail a takes-only inverse, and these are the assertions that make
it bite rather than pass vacuously:

- a column of ≥3 takes, **a non-default active take**, and a **comp map with a
  non-zero crossfade** on a boundary — a dropped xfade passes an
  `xfade=0` assertion, so the fixture must carry one (`comp_crossfade.qxa`
  shows both the spelling and an audibly different span);
- **takes that differ in the comped span.** Two identical takes make comp loss
  inaudible; use the `startOffset="96000"` sawtooth of
  `take_delete_refused_not_silent.qxa`, or `test_gapsaw.wav` as
  `comp_crossfade` does;
- `set-clip-fade clip= take=`, `set-clip-name take=` and a take-level
  `set-clip-pan` **before** the delete — each is state a clone drops and the
  pin keeps, and without them the case cannot tell the designs apart;
- on the wrapped configuration, the same for the **wrapper**;
- `render` before, `render` after undo, and **`assert-file-identical`** between
  them — byte-level, which is this project's exactness rule, and far stronger
  than per-span energy;
- `assert-take-column clip= shape= takes= activeTake= placements="1" comp=...`
  after the undo. `activeTake` matters independently: with a comp map covering
  the column, `takeFor` prefers the map, so a wrong `activeTake_` still renders
  identically. `placements="1"` catches a leaked pin;
- **undo → redo → undo → render → `assert-file-identical` again**, which is the
  only thing that catches the one-shot-inverse failure in §3;
- a `save-project` **while the delete is pending**, then a check on the file,
  for the §3 serialisation consequence;
- and the (b) refusal log **absent** for this shape
  (`minCount="0" maxCount="0"`) — otherwise the delete is still refused and
  every assertion above is vacuous.

Both shapes, direct and wrapped.

## 8. What this does not do

- It does not lift the refusal for a live recording (§4), which keeps it
  deliberately.
- **`take_delete_refused_not_silent.qxa` flips when this lands.** It asserts
  `expectReject="true"` and the refusal log on the direct shape. It must be
  retargeted to the recording shape or retired with a licence recorded beside
  it — not quietly deleted.
- It does not fix `split-clip`, `remove-take` or `duplicate-clip` (§6).
- It does not give `SCompositeAction` the ability to roll back a partially
  applied batch; (b) made that announced, and the note there stands.
- It does not make `SRestoreContainerClipAction` faithful, which it also is
  not — it drops volume, pan, fade, automation and name today. Cited in the
  first draft as a model; it is a precedent for the shape of the action, not
  for its fidelity.

## 9. Review history

Reviewed 2026-10-11 (Fable). **Verdict RED**, on the premise rather than the
details, and it was right.

- **§1's premise was inverted.** "The inverse cannot hold a reference, it has to
  hold a copy" — but `addRef()` is how you hold one, and
  `SRemoveTrackAction`/`SRestoreTrackAction` have done exactly this for tracks
  all along, with a comment that reads as if written for this problem. Rewritten
  around the pin.
- **The clone design could not have worked**, independently of fidelity: the
  undo command reuses the same inverse object, so a one-shot clone dangles on
  undo → redo → undo. The "delete on destruction without applying" lifecycle
  was wrong.
- **The §1 state table was incomplete.** It missed per-take fade, per-take name,
  MIDI take volume/pan, the wrapper's own parameters, `SLink`'s
  timebase/startTicks/eventChannelOverride, and the column's own attributes.
  My one suspicion going in — pan — turned out half right: audio takes carry
  pan through a clone, MIDI takes do not.
- **The wrapped-shape claim was wrong and over-broad**, and so was the claim
  about where `SMARAGD_TAKE_MIGRATE` is set. Both had been inferred from a
  single hand-run probe that was itself mis-configured.
- **"Project-parented is inert" was wrong**: `SProject::serialize` writes every
  child, so a pending object is saved. True of the existing track pin too, so
  it is stated as a property rather than treated as a new bug.
- **Recordings were settled** — refusal stays, for ownership, not cost.
- **The gate was weak enough to pass several wrong implementations.** §7 now
  names each one and the assertion that catches it.
- Also found: `split-clip` drops the tail's comp in both branches, and
  `remove-take` repeats QBX-146's exact null-inverse pattern. Both recorded in
  §6 rather than absorbed here.
