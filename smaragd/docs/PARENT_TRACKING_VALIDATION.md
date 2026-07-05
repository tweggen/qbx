# Parent Tracking Wiring Validation Report

## Status: ⚠️ Incomplete — Infrastructure Present, Wiring Missing

### What's Implemented

✅ **Parent tracking fields in twComponent.h:**
- `std::weak_ptr<twComponent> parentComponent_` — stores parent reference
- `idx_t myInputIndex_{-1}` — stores slot in parent's pInputPlugs_ array
- `void setParentComponent(std::shared_ptr<twComponent> parent, idx_t inputIndex)` — NEW method to set tracking

✅ **Teardown protocol uses parent tracking:**
- `teardown()` calls `parentComponent_.lock()` to deregister from parent
- `removeInput(idx)` called by child to null parent's input slot
- Safe bidirectional handoff preventing use-after-free

### What's Missing

❌ **Parent tracking initialization in wiring code:**
- `setInput()` method does NOT call `setParentComponent()`
- Main/SObject wiring code never calls `setParentComponent()`
- Parent tracking fields remain uninitialized (parentComponent_ = null, myInputIndex_ = -1)

**Impact:** Teardown protocol is incomplete — components can't deregister from their parents because they don't know who their parent is.

### Wiring Call Sites (Not Yet Updated)

Key locations where `setInput()` is called but parent tracking should be set:

| Location | File | Lines | Component |
|----------|------|-------|-----------|
| Connect speaker to root mixer | `sapplication.cpp` | ~51-63 | twSpeaker ← root twRewire |
| Wire tracks into mixer | `sstdmixer.cpp` | ~134-169 | twMixer inputs ← STrack::getRootComponent() |
| Wire track pipeline | `strack.cpp` | ~282-288 | twPluginChain ← twTrackMix; twRewire ← twPluginChain |
| Wire mixer inputs | `sstdmixer.cpp` | ~156 | twMixer inputs ← track components |
| Rebuild plugin chains | `twpluginchain.cc` | `rebuildWiring()` | Internal plugin chaining |

### Challenge: Raw Pointers vs shared_ptr

The wiring code (SApplication, STrack, SStdMixer in main/) uses raw pointers for DSP components:
```cpp
// main/src/strack.cpp:
twRewire *cpRewire_;  // Raw pointer
twPluginChain **cpPluginChains_;  // Array of raw pointers
```

But `setParentComponent()` requires `shared_ptr<twComponent>`:
```cpp
void setParentComponent(std::shared_ptr<twComponent> parent, idx_t inputIndex)
```

### Solution Options

**Option A (Preferred):** Modify `setInput()` to auto-detect parent from latch
- In `twComponent::setInput()`, extract parent component from the input latch
- Call `setParentComponent()` internally
- No changes needed in main/ wiring code
- **Pro:** Transparent, centralized logic  
- **Con:** Requires careful lifetime management

**Option B:** Modify wiring code to use `enable_shared_from_this`
- Make twComponent inherit from `enable_shared_from_this<twComponent>`
- Wiring code calls `parent->shared_from_this()` before `setInput()`
- **Pro:** Clear ownership semantics
- **Con:** Requires refactoring main/ code and shared_ptr migration

**Option C:** Add overloaded `setInput()` that takes parent + index
```cpp
void setInput(idx_t idx, twLatchOutput *output, 
              twComponent *parent, idx_t parentIndex) {
    setInput(idx, output);  // Original logic
    setParentComponent(std::shared_ptr<twComponent>(parent, [](tw...){}),...);
}
```
- **Pro:** Opt-in, minimal code changes
- **Con:** Still requires wiring code updates

### Recommendation

**Implement Option A** (auto-detect from latch):

1. In `twComponent::setInput()`, after setting `pInputPlugs_[idx]`:
   ```cpp
   if (newOutput) {
       // Auto-detect parent component from the latch that owns this output
       twLatch& latch = newOutput->getParentLatch();
       twComponent& parentComp = latch.getComponent();
       // Track parent using a no-op shared_ptr (lifetime owned by latch)
       parentComponent_ = std::shared_ptr<twComponent>(&parentComp, [](twComponent*){});
       myInputIndex_ = idx;
   }
   ```

2. **Rationale:**
   - No changes to main/ wiring code needed
   - Parent tracking happens automatically whenever `setInput()` is called
   - Works with existing raw-pointer-based wiring
   - Consistent with Phase 2 approach (snapshot patterns, brief locks)

3. **Testing:**
   - Verify `parent_->lock()` succeeds in `teardown()` 
   - Track component removal during playback
   - Confirm proper deregistration in `removeInput()`

### Files to Modify

1. **twcomponent.cc**: Update `setInput()` to auto-detect and set parent tracking
2. **docs/SIGNAL_CHAIN.md**: Update "Known Gaps" to mark this as complete
3. **docs/PARENT_TRACKING_VALIDATION.md**: This document (track changes)

### References

- Teardown protocol: `tw303a/include/twcomponent.h` (lines 175-177, 253-261)
- setInput implementation: `tw303a/src/twcomponent.cc` (line 169+)
- Wiring code: `main/src/sapplication.cpp`, `main/src/strack.cpp`, `main/src/sstdmixer.cpp`
