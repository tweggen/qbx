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
3. **Every `SSelectionManager` call that resolves a path threads `pathRoot_`
   through, including validation.** `SSetSelectionAction::apply()` used to
   call `isPathValid(path, project)` — omitting the action's own `pathRoot_`,
   so it validated an arrangement-tab path against the MASTER tree instead of
   the tab's own root (proposal 09 D21). It was invisible for a long time
   because the one existing cross-tab test (`tabs_selection_scoped.qxa`)
   happens to give its arrangement the SAME shape as the master, so the wrong
   root still validated by coincidence. `select_all_scope.qxa` (AC-a3) is the
   first case whose arrangement genuinely differs and would have failed on
   the pre-fix binary.
4. `SSelectionManager::allClipPaths(SObject *root)` (AC-a3) walks EVERY child
   — recursing into a lane (`SObject::isPathContainer()`) rather than
   counting it as a clip, indices matching `childAt()`/`indexOfChild()`
   exactly — so a path it returns resolves through the ordinary
   `SObjectPath` machinery like any other. It takes no root NAME because the
   caller already resolved one (`splacements::rootNamed`); this stays a pure
   tree walk with no selection-list or app-context coupling.

How to test: action_roundtrip_test covers the XML; test_selection_actions.cpp
is a parked (unbuilt) unit test awaiting a harness.

Known debt: selection STATE still lives on the application behind
SAppContext; a dedicated selection service would let the context shrink.
