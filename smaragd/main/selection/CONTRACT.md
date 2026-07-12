# app/selection — CONTRACT

Purpose: selection state (SSelectionManager: path↔link resolution) and the
selection actions (set/add/remove/toggle/clear).

Public headers: app/selection/*.h

Depends on (engine): tw/core, tw/graph (transitively). App edges:
{actions, model} — shell-free since Phase 6 (selection state is reached via
SAppContext) and track-free (generic path helpers live in
app/model/sobjectpath.h).

Invariants:
1. Selection is stored as SLink pointers but SERIALIZED as paths — actions
   round-trip through paths so undo works across structural changes.
2. Selection actions are undoable like everything else; clearing is an
   action, not a side effect.

How to test: action_roundtrip_test covers the XML; test_selection_actions.cpp
is a parked (unbuilt) unit test awaiting a harness.

Known debt: selection STATE still lives on the application behind
SAppContext; a dedicated selection service would let the context shrink.
