# app/actions — CONTRACT

Purpose: the command framework — SAction (apply → SApplyResult{applied,
inverse}), the string-keyed registry, submission queue, undo history
(rejectedCount included), QUndoStack bridge — plus generic verbs
(set-property, grid/cycle/metronome/snap toggles, toggle-playback,
load-project, render).

Public headers: app/actions/*.h. The verb reference is docs/ACTIONS.md.

Depends on (engine): tw/core, tw/graph, tw/playback, tw/render. App edges:
per tools/check_layering.py.

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
