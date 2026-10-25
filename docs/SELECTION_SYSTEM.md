# Action-Backed Multi-Selection System

## Overview

Smaragd's multi-selection system is built on the same action/undo architecture as all other editable operations. This enables:
- **Undoable selection**: Each selection change can be undone/redone
- **Scriptable selection**: Selection operations can be recorded in `.qxa` action scripts
- **Serializable state**: Selection actions serialize to XML for persistence and replay
- **Consistent UI semantics**: Click gestures (plain, Shift, Ctrl) automatically batched as single undoable actions

## Architecture

### Components

**SSelectionManager** (`include/sselectionmanager.h`)
- Converts between live `SLink*` pointers and index paths for serialization
- `linksToPaths()` — snapshot selection to paths (for XML export)
- `pathsToLinks()` — resolve paths back to live `SLink*` objects (for undo replay)
- `isPathValid()` — validate path existence before applying actions

**Selection Action Classes** (`include/actions/s*selectionaction.h`)
- `SSetSelectionAction` — clear all, set to given paths (inverse: `SSetSelectionAction` with prior)
- `SAddToSelectionAction` — add paths to existing selection (inverse: `SRemoveFromSelectionAction`)
- `SRemoveFromSelectionAction` — remove paths from selection (inverse: `SAddToSelectionAction`)
- `SClearSelectionAction` — deselect all items (inverse: `SSetSelectionAction` with prior)
- `SToggleSelectionAction` — toggle each path in/out of selection (inverse: self)

### Index Path Format

Index paths use **strackpath** notation: pipe-separated comma-separated indices.

Example: `"0,1|1,2|2,0"`
- `0,1` — track 0, clip at position 1
- `1,2` — track 1, clip at position 2
- `2,0` — track 2, clip at position 0

Paths serialize to XML attributes and `.qxa` action scripts as comma-separated, pipe-delimited strings.

## UI Integration

### Click Gesture Mapping

`sstdmixerview.cpp` routes mouse clicks to action submission:

| Gesture | Action |
|---------|--------|
| Plain click | `submitSetSelectionAction(link)` → clears, selects one |
| Shift+click | `submitToggleSelectionAction(link)` → toggle in/out |
| (future) Ctrl+click | `submitAddSelectionAction(link)` → add without clearing |

### SApplication Convenience Methods

```cpp
// Submit action for single SLink* (convenience over path-based API)
void submitSetSelectionAction(SLink *link);
void submitAddSelectionAction(SLink *link);
void submitToggleSelectionAction(SLink *link);
void submitClearSelectionAction();

// Path-based API (for script loading, batch operations)
void setSelectionFromPaths(const QList<QList<int>> &paths);
void addSelectionFromPaths(const QList<QList<int>> &paths);
void removeSelectionFromPaths(const QList<QList<int>> &paths);
void toggleSelectionFromPaths(const QList<QList<int>> &paths);
QList<QList<int>> getCurrentSelectionPaths() const;
```

## XML Serialization

All selection actions implement `writeXml()` and `readXml()` for `.qxa` action script support.

**Example `.qxa` script:**
```xml
<?xml version="1.0" encoding="UTF-8"?>
<script version="1">
  <actions>
    <!-- Clear existing selection -->
    <action type="clear-selection" />
    <!-- Select clips at track 0, positions 0 and 1 -->
    <action type="set-selection" paths="0,0|0,1" />
    <!-- Add clips from other tracks -->
    <action type="add-to-selection" paths="1,0|2,1" />
  </actions>
</script>
```

## Undo/Redo Mechanics

Each action's `apply()` method:
1. **Validates** all paths before mutating state
2. **Snapshots** current state (for inverse)
3. **Applies** the operation (calls path-based `SApplication` methods)
4. **Returns** `SApplyResult{true, inverseAction}`

On undo, the inverse action is applied with the same semantics, restoring prior state.

### Inverse Action Chaining

- `SetSelection` → inverse is `SetSelection` with prior paths
- `AddToSelection` → inverse is `RemoveFromSelection` with same paths
- `RemoveFromSelection` → inverse is `AddToSelection` with same paths
- `ClearSelection` → inverse is `SetSelection` with prior paths
- `ToggleSelection` → inverse is `ToggleSelection` (self-inverse)

## Testing

Unit tests in `src/actions/test_selection_actions.cpp` verify:
- Action creation and naming
- XML serialization round-trip
- Inverse action generation
- Undo/redo chain consistency

Example test usage:
```cpp
QList<QList<int>> paths = { {0,0}, {0,1} };
SSetSelectionAction action(paths);
SApplyResult result = action.apply(project);
QVERIFY(result.success);
QCOMPARE(result.inverse->name(), "set-selection");
```

## Selection is per root, and the lifetime rule that follows (QBX-230)

This document described a single selection list. It is not one: since proposal
09 §3 the selection is **one list per arrangement root**, and each list
persists while its root is not the active one, so switching tabs and switching
back finds the selection as it was.

```cpp
struct SSelectionEntry { SSelectionList list; SLink *current = nullptr; };
QHash<QString, SSelectionEntry> selections_;   // keyed by ARRANGEMENT NAME
QString                         activeSelectionRoot_;
SSelectionList                 *selectionList_;  // the ACTIVE entry's list
```

The master root's key is `QString()`. Note that `SProject::arrangementNameOf`
returns `QString()` for a root it cannot find, so an unnameable or
already-unregistered root **aliases onto the master's list**; and an action
that sets no path root means the master too (`saction.h`: "empty == the master
root"), which is why `SApplication::submitSetSelectionAction` clears the
master's selection whatever tab the click happened in.

### A destroyed link must be purged from EVERY root

`addSelectedSLink` connects the link's `destroyed()` signal **once**, with no
record of which root it was selected under. The handler therefore cannot use
the active root to decide where to clean up, and must remove the pointer from
every list:

* `SApplication::purgeDestroyedLink` removes the dying link from every
  `selections_` entry and from every `current`, **by pointer value**,
  dereferencing nothing — the rule `docs/PHASE3_SAFETY_ADDENDUM.md` already
  states for `SObject::dependentLinks_`.
* It must **not** call `QObject::disconnect` on the dying object. The
  connection dies with it, and that call is what faulted in QBX-230.
* The user-level `unselectSLink( SLink* )` *does* disconnect, because its link
  is alive — but only once no root still carries it, since the wire is per
  link rather than per root.

**Why the active root cannot be trusted here, concretely.** Qt emits
`destroyed()` before deleting an object's children. Destroying an arrangement
root reaches `SViewTabs::onRootDestroyed`, which calls `removeTab`, which makes
`QTabWidget` emit `currentChanged`, which calls `setActiveSelectionRoot`. So
the root's own handler re-points the active list *before* the links it owns
announce their deaths:

```
~QObject(root)
  emit destroyed()  ->  onRootDestroyed -> removeTab -> currentChanged
                            -> setActiveSelectionRoot( OTHER )
  delete children   ->  each selected SLink's destroyed()
                            -> purge aimed at the WRONG list
```

A single-root purge therefore fails **by construction** whenever a root holding
a selection is destroyed, not merely when a user happens to be on another tab.

### The registry is dropped at a document boundary

`selections_` is keyed by arrangement name and lives outside the object tree,
so it is exactly what `SProject::clearArrangements` warns about: *"anything
held in a by-name registry rather than in the object tree has to be dropped
explicitly or it is inherited by the document being opened"*. A project swap
therefore calls `SApplication::clearAllSelections()` from
`setCurrentProject`. This is a second, independent defence: the purge handles
destruction in general, this handles inheritance across documents — which
matters because a reopened file re-registers the **same names**.

### An unregistered arrangement takes its selection entry with it

A project swap is not the only document boundary, and it is not the one a
**load** crosses. `SLoadProjectAction` loads *into* the same `SProject`, so
`setCurrentProject` is never called with a different pointer and
`clearAllSelections()` does not fire. What the load does call is
`clearArrangements()`, which unregisters every name and then lets
`createObjects()` register them again — so a `selections_` entry left behind is
inherited by the **next root of the same name**.

`SApplication` therefore listens to `SProject::arrangementUnregistered` and
clears that root's entry. The signal had been declared and emitted since the
first commit with nothing connected to it; this is its first listener.

Note what this defect is *not*. The old root survives its unregistration,
because an open arrangement tab holds a ref and `unregisterArrangement`'s
`removeRef()` need not reach zero. So the stranded link is **alive but
orphaned**, not freed — measured as the empty path `"Drums:"`, empty because
the link is no longer reachable in the registered tree. The purge cannot help
here precisely because nothing was destroyed: an entry that survives a load
belongs to a link that never died.

**One half of this is still open.** The *master* root is never unregistered, so
no signal covers it, and the measured state after a load is that its entry
still holds one orphaned link. The gate asserts neither value for it — `0`
fails and `1` would pin the staleness as intended behaviour — and the case says
so in place.

### What is gated

`smaragd/tests/cases/selection_purge_across_roots.qxa` carries the measured
sabotage table in its header: the cross-root purge, the duplicate guard, the
per-root `current` update and the `arrangementUnregistered` listener are each
pinned, and `clearAllSelections()` at a project swap is **not** — no `.qxa` can
reach it, for the reason given there. It stays in regardless; the sabotage shows
only that no script reaches it, not that anything else covers it.

## Known Limitations & Future Work

1. **Ctrl+Click**: Currently mapped to plain click. Should add `SAddToSelectionAction` when Ctrl+click support is enabled.
2. **Range selection**: Shift+click on two items to select range not yet implemented.
3. **Context menu**: No context menu for "Select All", "Deselect All", etc. (future: can submit actions from menu).
4. **Gesture batching**: Multiple clicks within ~100ms could be grouped into single action (future refinement).

## Integration with Action Scripts

Selection actions integrate seamlessly with the `.qxa` action script format:

**Recording:** When user performs selection, action is automatically recorded to undo history (and can be exported to `.qxa`)

**Playback:** Load a `.qxa` script containing selection actions; they replay identically via path resolution

**Scripting:** Users can hand-author `.qxa` files with selection sequences for regression testing or automation

See `examples/selection_demo.qxa` for example script.
