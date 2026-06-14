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
