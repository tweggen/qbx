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

**`metronome-toggle/-enable/-disable` stopped being a stub in proposal 21 L5**,
without one line of this module changing. They still only flip
`SProjectProps::Metronome`; what is new is that `SApplication` watches that
property and turns it into a live-plan rebuild, so the click joins the live lane
while the transport rolls. That is the shape a toggle verb should have — the
verb owns the STATE, the shell owns what the state MEANS — and it is why the
action needed no knowledge of the pump, the plan or the audio device.
