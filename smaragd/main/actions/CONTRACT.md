# app/actions — CONTRACT

Purpose: the command framework — SAction (apply → SApplyResult{applied,
inverse}), the string-keyed registry, submission queue, undo history
(rejectedCount included), QUndoStack bridge — plus generic verbs
(set-property, set-project-channels, grid/cycle/metronome/snap toggles,
toggle-playback, load-project, render).

Public headers: app/actions/*.h. The verb reference is docs/ACTIONS.md.

Depends on (engine): tw/core, tw/graph, tw/render. App edges: {model}
ONLY (Phase 6): the framework reaches the app via SAppContext, playback
toggling goes through SAppContext::setPlaybackRunning (no engine speaker
include), and load-project moved to persistence.

Invariants:
1. Every user-visible mutation is an SAction; apply() returns the INVERSE
   action (or null for non-undoable) — never mutate outside apply().
2. Actions are XML-round-trip stable (writeXml/readXml; roundtrip test);
   registration is via static initializers — the app MUST stay an OBJECT
   library or these TUs vanish at link (see main/CMakeLists.txt).
3. Rejected apply() increments SActionHistory::rejectedCount(); the test
   runner turns that into failures (testkit CONTRACT).
4. Positions/durations in XML are FRAMES; paths are comma-separated indices.
5. Submission is synchronous today (drain on the GUI thread); do not assume
   that in action code — it may move to the engine thread (Phase 2 of
   proposal 03).
6. **THE ROOT AN ACTION'S PATHS ADDRESS LIVES IN `SAction::pathRoot_`, AND
   NOWHERE ELSE** (proposal 09 D21; enforced by `check_pathroot` job 2 since
   QBX-249). Two funnels read that one member and nothing else:
   `applyPropagatingRoot()` copies `a->pathRoot()` onto every inverse — the
   one place that rule lives for all 83 inverse-construction sites in the
   tree — and `setPathRoot()` is what `stimeline::submitFor()`,
   `SMixerStrip::submit_()` and `SEventPassVerbs::emitVerb()` stamp an action
   with. An action that keeps its root in a member of its own is invisible to
   both, **and both failures are silent**: `add-track` held an
   `arrangement_`, so the `SRemoveTrackAction` it handed back was
   master-rooted and one undo of an arrangement-side `add-track` DELETED A
   MASTER LANE while leaving the arrangement's standing, and `setPathRoot()`
   was ignored so "New track" in an arrangement tab added the lane to the
   master. A base member cannot go in a member-init list, so a constructor
   taking an `arrangement` argument calls `setPathRoot()` in its BODY. Read
   `pathRoot()` rather than redeclaring either — a derived `pathRoot_` or
   `pathRoot()` SHADOWS the base's non-virtual pair, which `merge-columns`
   and `set-lane-hidden` both did.

How to test: action_roundtrip_test.exe; every qxa case exercises the
submit/apply/reject path.

Known debt: sactionhistory reaches SApplication for the current project
(shell edge); expectReject is per-element, not per-verb.

**`SAction::marksProjectClean()` (2026-08-23, default `false`) is an opt-in
hook for exactly one thing today**: `SSaveProjectAction` overrides it `true`,
and `SActionHistory::onApplied_()` calls `QUndoStack::setClean()` for any
applied action that returns true from it, AFTER any inverse has been pushed (a
`setClean()` before the push marks the position the push then leaves BELOW the
new command — dirty again immediately; irrelevant while the only override is
non-undoable, but the ordering is correct regardless). It exists here rather
than as a `name() == "save-project"` string check in the history, for the same
reason `mergeKey()`/`knownAttributes()` are hooks rather than special cases:
the history owns the `QUndoStack` and applies every action the SAME way,
scripted (`<save-project>` in a `.qxa`) or interactive (the menu's Save, which
does NOT go through this history — see `main/shell/CONTRACT.md` inv. 51 — and
calls `setClean()` by hand for exactly that reason). Before this, `setClean()`
was called NOWHERE in this repository, so a project's undo stack never went
clean after a save and `hasUnsavedChanges()` stayed true forever past the
first edit.

**A MERGE KEY MUST CARRY A GESTURE, NOT JUST A CONTROL (QBX-206).** There are
TWO merge layers and they do not bound the same thing:

- `SActionQueue::enqueue` merges only with the **pending tail**, so it is
  bounded by the drain. This is the layer that actually implements "one drag is
  one undo step".
- `QUndoStack::push` folds a new command into the one **on top of the stack**
  whenever `SActionUndoCommand::id()` (`qHash(mergeKey())`) matches, with **no
  time and no gesture boundary at all**.

So a key built from the control's identity alone — a path, a path plus a
parameter, or a constant — merges every edit that control ever receives, for
the lifetime of the stack. Two separate fader drags became ONE undo entry, and
because the stack was then one entry short, the undo after them took back the
action BENEATH them (`control_edit_undo_steps.qxa` pins both halves).

Every `mergeKey()` therefore ends with `gestureKeySuffix()`, and every
`mergeWith()` begins by refusing `!sameGesture( o )`. **Both**, redundantly:
QBX-203 measured that reverting either site alone leaves every gate passing,
because each one prevents the merge on its own — so a single-site regression
must not be able to restore the old behaviour quietly.

`SAction::gesture_` is **unique by default**, which is the safe direction and
also why almost nothing needs wiring: an action nobody stamped merges with
nothing, so a dialog, a combo box, a wheel notch, a double-click reset or a
scripted verb is one committed value and therefore one undo step — which is
what each of them already meant. Only a control that STREAMS through one
gesture has anything to say, and it says it by minting a token on
`sliderPressed` and clearing it on `sliderReleased`.

**THE TWO MERGE SITES NOW AGREE ON AN EMPTY KEY (QBX-206).** They used not to:
`SActionUndoCommand::id()` returns `-1` for an empty key, meaning "never
merge", while `SActionQueue::enqueue` compared keys for equality and **two
empty keys are equal** — so an action that overrode `mergeWith()` but no
`mergeKey()` would merge in the queue and never on the stack. It was harmless
only because `SAction::mergeWith()` defaults to `false`, i.e. because of a
second, unrelated default. The queue now skips the merge for an empty key.

**`metronome-toggle/-enable/-disable` stopped being a stub in proposal 21 L5**,
without one line of this module changing. They still only flip
`SProjectProps::Metronome`; what is new is that `SApplication` watches that
property and turns it into a live-plan rebuild, so the click joins the live lane
while the transport rolls. That is the shape a toggle verb should have — the
verb owns the STATE, the shell owns what the state MEANS — and it is why the
action needed no knowledge of the pump, the plan or the audio device.
