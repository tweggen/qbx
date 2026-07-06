# Unified Rendering Architecture V3: Teardown Protocol

## Overview

This document describes the component teardown protocol that enables safe, lock-free removal of components from the DSP graph while the audio thread renders continuously without interruption.

## Core Principles

1. **Audio thread continuity**: Deletion of any component (track, effect, clip) must not block the audio thread
2. **Mathematical correctness**: Removing a component should only affect audio output by its mathematical absence (output silence)
3. **DAG topology awareness**: Components can have multiple parents; each parent relationship must be severed independently
4. **Bidirectional handoff**: Child components initiate their own deregistration from parents

---

## Component Lifecycle States

```cpp
enum class ComponentState {
    ACTIVE,    // Normal operation
    ZOMBIE,    // Tearing down, outputs silence, may be referenced by audio thread
    DELETED    // Memory freed, no valid references exist
};
```

**State Transitions**:
- `ACTIVE → ZOMBIE`: Initiated by `teardown()` call, immediate
- `ZOMBIE → DELETED`: After `teardown()` completes and all references released
- Audio thread checks state with acquire semantics; render path is a fast path for ZOMBIE

---

## Teardown Protocol: Three Phases

### Phase 1: Mark and Disconnect (Non-blocking, < 1µs)

```cpp
void twComponent::teardown() {
    // 1a. Mark self as ZOMBIE immediately
    // Audio thread will see this next time it renders
    state_.store(ComponentState::ZOMBIE, std::memory_order_release);
    
    // 1b. Self-deregister from parent (bidirectional handoff)
    // Child removes itself; parent doesn't forcibly eject
    if (parentComponent_) {
        parentComponent_->removeInput(myInputIndex_);
    }
    
    // 1c. Notify dependents this component is being torn down
    // They should stop expecting data or prepare for silence
    for (auto dep : dependents_) {
        dep->onDependencyTeardown(this);
    }
    
    // [Phase 2 follows - see below]
}
```

**Why this is fast:**
- No locks held (except in `removeInput()` for microseconds)
- Audio thread sees ZOMBIE state immediately
- `calcOutputTo()` returns silence without recursing
- Dependent notifications are async-friendly

### Phase 2: Snapshot Children (Non-blocking, brief lock)

```cpp
    // [Phase 1 complete]
    
    // 2a. Snapshot children under brief lock
    // Copy shared_ptrs so children stay alive during recursion
    std::vector<std::shared_ptr<twComponent>> childrenCopy;
    {
        std::lock_guard<std::mutex> lock(mutex());
        for (auto& child : children_) {
            childrenCopy.push_back(child);  // Increment refcount
        }
    }  // Lock released - audio thread can proceed
    
    // 2b. Recursive teardown (non-blocking)
    // Each child removes itself from this component's input list
    for (auto& child : childrenCopy) {
        child->teardown();
    }
    // childrenCopy goes out of scope, refcounts decrement
    // Children are freed once no references remain
}
```

**Why this is safe:**
- Lock held only while copying shared_ptrs (~1-10µs)
- Audio thread can acquire lock immediately after
- Recursion happens without locks
- Shared_ptr semantics guarantee children don't vanish during iteration

### Phase 3: Actual Deletion (Deferred)

```cpp
// After teardown() returns, component still exists
// But is unreachable from audio path (deregistered from parents)
// Memory is freed when:
//   - Last external reference is released
//   - Or UI layer explicitly calls delete
// 
// Key: deletion happens AFTER teardown(), not during
```

---

## Audio Thread Integration

### Fast Path: Zombie Check

```cpp
length_t twComponent::calcOutputTo(IOVector& dest, idx_t idx) {
    // Check state with acquire semantics
    // This synchronizes with teardown()'s release store
    if (state_.load(std::memory_order_acquire) == ComponentState::ZOMBIE) {
        return dest.fillSilence(0, dest.length());
    }
    
    // Normal render path (unchanged)
    return calcOutputTo_impl(dest, idx);
}
```

**Performance**: 
- Atomic load is single CPU instruction
- Branch prediction: path stays predictable (either fully active or fully silent)
- No memory allocation or locking

### Input Validation: Handling NULL Inputs

```cpp
// After parent calls removeInput(idx), that slot is nullptr
// Components must handle this gracefully:

void twMixer::calcOutputTo(IOVector& dest, idx_t idx) {
    // In snapshot loop:
    for (idx_t ch = 0; ch < mixerInputs_; ++ch) {
        if (ch < (idx_t)pInputPlugs_.size() && pInputPlugs_[ch]) {
            // Input exists and is valid
            realRead = static_cast<twLatchStreamingOutput*>
                (pInputPlugs_[ch].get())->readStreamingData(...);
        } else {
            // Input is NULL (being torn down)
            // Mixer just doesn't mix this channel
        }
    }
    // Output is mixture of remaining inputs
    // Effectively the removed track's "silence" is added
}
```

---

## Dependency Notification Protocol

Components with external dependents must notify them during teardown:

```cpp
// In twComponent:
virtual void onDependencyTeardown(twComponent* dep) {
    // Called when a dependency is being torn down
    // Default: ignore (component handles NULL inputs gracefully)
    // Override: cleanup, adjust state, notify UI
}

// Example: twPluginChain when a plugin is removed
void twPluginChain::onDependencyTeardown(twComponent* dep) {
    std::lock_guard<std::mutex> lock(pluginsMutex_);
    // Remove plugin from plugins_ list
    plugins_.erase(
        std::remove_if(plugins_.begin(), plugins_.end(),
            [dep](auto* p) { return p == dep; }),
        plugins_.end()
    );
}
```

---

## Component Types and Their Teardown

### twRewire (Track Router)

```cpp
void twRewire::teardown() override {
    state_.store(ComponentState::ZOMBIE, std::memory_order_release);
    
    // Deregister from parent mixer
    if (parentComponent_) {
        parentComponent_->removeInput(myInputIndex_);
    }
    
    // Notify all consumers I'm gone
    for (auto dep : dependents_) {
        dep->onDependencyTeardown(this);
    }
    
    // Snapshot and tear down all track inputs
    std::vector<std::shared_ptr<twComponent>> inputsCopy;
    {
        std::lock_guard<std::mutex> lock(mutex());
        for (auto& input : pInputPlugs_) {
            if (input) inputsCopy.push_back(input);
        }
    }
    for (auto& input : inputsCopy) {
        input->teardown();
    }
}
```

### twTrackMix (Track Mixer with Clips)

```cpp
void twTrackMix::teardown() override {
    state_.store(ComponentState::ZOMBIE, std::memory_order_release);
    
    if (parentComponent_) {
        parentComponent_->removeInput(myInputIndex_);
    }
    
    for (auto dep : dependents_) {
        dep->onDependencyTeardown(this);
    }
    
    // Snapshot clips and tear them down
    std::vector<std::shared_ptr<twComponent>> clipsCopy;
    {
        std::lock_guard<std::mutex> lock(mutex());
        for (auto& clip : clips_) {
            if (clip.view) clipsCopy.push_back(clip.view);
        }
        clips_.clear();  // Prevent audio thread from iterating
    }
    for (auto& clip : clipsCopy) {
        clip->teardown();
    }
}
```

### twPluginChain (Effect Chain)

```cpp
void twPluginChain::teardown() override {
    state_.store(ComponentState::ZOMBIE, std::memory_order_release);
    
    if (parentComponent_) {
        parentComponent_->removeInput(myInputIndex_);
    }
    
    for (auto dep : dependents_) {
        dep->onDependencyTeardown(this);
    }
    
    // Tear down each plugin in order
    std::vector<std::shared_ptr<audio::twPluginInsert>> pluginsCopy;
    {
        std::lock_guard<std::mutex> lock(pluginsMutex_);
        pluginsCopy = plugins_;
        plugins_.clear();
    }
    for (auto& plugin : pluginsCopy) {
        plugin->teardown();
    }
}
```

---

## Eliminating Borrowed Pointer Issues

### The Problem (Before)

```cpp
// Parent holds borrowed pointers to children
std::vector<twComponent*> children_;  // Raw pointers, no ownership

// When child is deleted:
// - Parent still references child in children_ → dangling pointer
// - Audio thread accesses dangling pointer → crash or UB
```

### The Solution (After)

```cpp
// Phase 1: Child deregisters from parent
child->teardown();
  └─> parent->removeInput(childIdx);
      └─> pInputPlugs_[childIdx] = nullptr;  // Atomic disconnect

// Phase 2: Child marks self as ZOMBIE
      └─> state_ = ZOMBIE;

// Audio thread:
if (state_ == ZOMBIE) return silence;  // Fast path, no dereferencing
```

**Result**: No borrowed pointer issues because:
1. Parent removes reference before child is deleted
2. Audio thread checks component state, not parent's list
3. Dependents are notified via callback, not by discovering NULL

---

## Track Removal Flow (Complete Example)

```
UI Thread: SStdMixer::removeTrack(trackIndex)

1. Get track's root component (twRewire)
2. Call twRewire->teardown()
   ├─ Phase 1: Mark ZOMBIE, deregister from SStdMixer::twMixer
   │   └─ twMixer->removeInput(trackIndex)
   │      └─ pInputPlugs_[trackIndex] = nullptr  [ATOMIC]
   │   └─ Notify dependents
   │
   ├─ Phase 2: Snapshot children (twPluginChain, etc.)
   │   └─ foreach child: child->teardown()  [RECURSIVE]
   │       ├─ twPluginChain->teardown()
   │       │   └─ foreach plugin: plugin->teardown()
   │       │       └─ Plugins mark ZOMBIE
   │       │
   │       └─ twTrackMix->teardown()
   │           └─ foreach clip: clip->teardown()
   │               └─ Clips mark ZOMBIE
   │
   └─ Phase 3: (implicit) References released, components freed

3. Delete SLink (UI-layer track wrapper)
4. Return to event loop

Audio Thread (concurrent, unblocked):
  During Phase 1: Sees NULL input slot, outputs silence automatically
  During Phase 2: Sees ZOMBIE state on plugins/clips, fills silence
  After Phase 3: Track is gone, mixer routes around it
  
Result: Playback continues smoothly; only silent audio for removed track
```

---

## Guarantees and Invariants

### Guaranteed Properties

1. **No blocking on audio thread**: teardown() acquires locks only for microseconds
2. **No use-after-free**: Parent reference removed before child is freed
3. **No dangling pointers in pInputPlugs_**: Set to nullptr atomically
4. **Audio output is correct**: Missing track outputs silence; other tracks unaffected
5. **No memory leaks**: Shared_ptr semantics guarantee cleanup

### Maintained Invariants

1. **Zombie components output silence**: Audio thread checks state before rendering
2. **DAG topology is preserved**: Each parent-child link is severed independently
3. **Dependent notification happens first**: Downstream components notified before children torn down
4. **Snapshot pattern holds everywhere**: Lock held only during copy, released before recursion

---

## Implementation Checklist

Components that need `teardown()` implementation:

- [ ] twComponent (base: mark ZOMBIE, notify dependents, deregister from parent)
- [ ] twRewire (tear down all input connections)
- [ ] twMixer (clear input slots, tear down inputs)
- [ ] twTrackMix (clear clips, tear down each clip)
- [ ] twPluginChain (clear plugins, tear down each plugin)
- [ ] twPluginInsert (mark ZOMBIE)
- [ ] twSampleReader (mark ZOMBIE)
- [ ] twWavInput (mark ZOMBIE)
- [ ] twView (forward to underlying component)
- [ ] twLatch and subclasses (minimal: mark ZOMBIE)

Methods that need updates:

- [ ] `calcOutputTo()`: Check ZOMBIE state with acquire semantics
- [ ] `removeInput(idx)`: Set slot to nullptr atomically
- [ ] `onDependencyTeardown()`: Handle dependency notifications
- [ ] UI layer: Call `teardown()` instead of direct delete

---

## Performance Characteristics

| Operation | Time | Lock Duration | Audio Impact |
|-----------|------|---|---|
| teardown() Phase 1 (mark + deregister) | <1µs | <10µs | None after ~10µs |
| teardown() Phase 2 (recursive) | Per-child | Released between recursion | None |
| calcOutputTo() zombie check | 1 CPU cycle | 0 | None |
| Audio output during removal | Continuous | N/A | Track outputs silence only |

---

## Related Issues Solved

1. **Borrowed pointer semantics**: Components don't "own" their dependents; teardown() notification replaces ownership confusion
2. **Race conditions on pInputPlugs_**: NULL assignment is atomic; ZOMBIE check is separate
3. **Audio thread blocking**: All locks released before recursive operations
4. **Multi-parent DAG removal**: Each parent-child link severed independently via `removeInput()`

