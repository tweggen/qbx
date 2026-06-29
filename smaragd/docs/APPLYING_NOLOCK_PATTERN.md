# Applying the _nolock() Pattern to Derived Components

## Overview

This document describes the systematic application of the _nolock() pattern (documented in `COMPONENT_LOCKING_STRATEGY.md`) to four derived component classes that were previously missing thread-safety protections. These classes exhibited critical race conditions due to unprotected shared state mutations.

## Background

The _nolock() pattern provides:
- **Explicit lock scope** — Callers know when locks are held
- **Deadlock prevention** — Recursive operations don't try to acquire held locks
- **Design clarity** — Forces consideration of what needs locking
- **Use-after-free prevention** — Longer critical sections protect entire operations

## Critical Race Conditions Fixed

### twMixer (Most Severe)
**Problem:** `calcOutputTo()` and `seekTo()` read `pInputPlugs` array while `setNInputs()` reallocates it
- T0: Audio thread reads `pInputPlugs[ch]` bounds check
- T1: UI thread calls `setNInputs()`, reallocates `pInputPlugs`
- T2: UI thread frees old array
- T3: Audio thread dereferences freed pointer → **USE-AFTER-FREE CRASH**

**Problem:** `setBufferSize()` reallocates `inBuffer` without lock, then `calcOutputTo()` reads it
- Race between allocation and first use → **BUFFER OVERFLOW**

**Solution:**
- Added `setNInputs_nolock()`, `calcOutputTo_nolock()`, `seekTo_nolock()`, `setBufferSize_nolock()`
- All public methods acquire `mutex()`, call `_nolock()` variant
- Lock spans entire operation to prevent races during array reallocation

### twRewire (Most Severe)
**Problem:** `setNPlugs()` deletes `twStreamingLatch` objects while consumers hold references
- T0: Audio thread gets reference to `pOutputLatches[idx]`
- T1: UI thread calls `setNPlugs()`, deletes latch at that index
- T2: Audio thread calls method on deleted object → **CRASH**

**Problem:** `calcOutputTo()` reads `pInputPlugs` array while `setNPlugs()` reallocates it
- Same use-after-free race as twMixer

**Problem:** `linkOutput()` accesses `pOutputLatches` array without lock
- Concurrent `setNPlugs()` may reallocate array → **USE-AFTER-FREE**

**Solution:**
- Added `setNPlugs_nolock()`, `calcOutputTo_nolock()`, `seekTo_nolock()`, `linkOutput_nolock()`
- Lock spans entire operation including latch deletion to prevent dangling references
- `linkOutput()` critical: must hold lock while accessing and calling methods on latches

### twSampleReader (Moderate)
**Problem:** `seekTo()` modifies `pos_` while `calcOutputTo()` reads it concurrently
- T0: Audio reads `pos_` for source offset
- T1: UI calls `seekTo()`, changes `pos_`
- T2: Audio advances `pos_` by length read → **AUDIO CORRUPTION / POSITION MISMATCH**

**Problem:** `captureInternalState()` and `restoreInternalState()` access `pos_` without lock
- State snapshot operations may race with active playback

**Solution:**
- Added `seekTo_nolock()`, `calcOutputTo_nolock()`, `reset_nolock()`
- All public methods acquire lock
- `captureInternalState()` and `restoreInternalState()` acquire locks to protect state consistency

### twWavInput (Moderate)
**Problem:** `seekTo()` modifies `playOffset_` while `calcOutputTo()` reads it
- Same race as twSampleReader but with playback offset

**Problem:** `getSource()` calls `viewAtRate()` which has TOCTOU race
- Multiple threads may create resampled view concurrently

**Solution:**
- Added `seekTo_nolock()`, `calcOutputTo_nolock()`, `reset_nolock()`
- All public methods acquire lock
- `reset()` now properly resets `playOffset_` under lock (was previously a no-op)

## Implementation Pattern Applied

For each class, the refactoring follows this structure:

### 1. Header Changes
```cpp
// twXxx.h
private:
    // Helper methods assume mutex is held
    int method_nolock(int arg);
    
    // Shared mutable state
    int sharedState_;
```

### 2. Implementation Changes
```cpp
// twXxx.cc

// Public: acquires lock, calls helper
int twXxx::method(int arg)
{
    std::lock_guard<std::mutex> lock(mutex());
    return method_nolock(arg);
}

// Private: does work under assumed lock
// Caller must hold mutex()
int twXxx::method_nolock(int arg)
{
    sharedState_ = arg;
    return 0;
}
```

### 3. Lock Scope
- Lock acquired only for the public method
- Entire operation protected: bounds check + array access + modification
- Private `_nolock()` variant always called while lock is held
- Documentation explains: "Caller must hold mutex()"

## Files Modified

### Headers
- `tw303a/include/twsamplereader.h` — added 3 `_nolock()` helpers
- `tw303a/include/twwavinput.h` — added 3 `_nolock()` helpers
- `tw303a/include/twmixer.h` — added 4 `_nolock()` helpers
- `tw303a/include/twrewire.h` — added 4 `_nolock()` helpers

### Implementations
- `tw303a/src/twsamplereader.cc` — refactored `seekTo()`, `calcOutputTo()`, `reset()`, capture/restore state
- `tw303a/src/twwavinput.cc` — refactored `seekTo()`, `calcOutputTo()`, `reset()`
- `tw303a/src/twmixer.cc` — refactored `seekTo()`, `calcOutputTo()`, `setNInputs()`, `setBufferSize()`
- `tw303a/src/twrewire.cc` — refactored `seekTo()`, `calcOutputTo()`, `setNPlugs()`, `linkOutput()`

## Critical Sections Protected

### twSampleReader/twWavInput
```cpp
// Audio position state is protected during
// - seeking (UI thread operation)
// - reading (audio thread operation)
// - state capture/restore (freezing thread operation)
```

### twMixer
```cpp
// Array reallocation protected:
// - pInputPlugs array (bounds-checked in calcOutputTo)
// - inputProperties array (accessed in calcOutputTo)
// - inBuffer (read in calcOutputTo)
```

### twRewire
```cpp
// Most critical: latch lifecycle protected
// - pOutputLatches allocation/deallocation (linkOutput reads)
// - pInputPlugs allocation/deallocation (calcOutputTo reads)
// - latch object destruction (prevents dangling refs)
```

## Lock Contention Considerations

### Current Design (This Refactoring)
- Lock held for entire public method (including I/O, memory allocation)
- Simple, correct, prevents use-after-free
- May cause brief audio thread stalls if UI thread allocates large array
- Acceptable because these operations are infrequent (UI-driven)

### Future Optimization (NOT in this change)
Could reduce lock scope by:
1. Allocate new arrays/buffers outside lock
2. Acquire lock only to swap pointers
3. Delete old arrays after releasing lock
4. Requires careful ordering to prevent use-after-free

Deferring this optimization because:
- Current lock scope is correct and safe
- Operation frequency is low (not realtime)
- Premature optimization adds complexity
- Can be added later if profiling shows contention

## Testing Recommendations

### Automated Tests
1. **Concurrent access test** — Spawn UI thread calling `setNInputs()` while audio thread calls `calcOutputTo()`
2. **Seek stress test** — Rapid `seekTo()` calls from UI while audio reads
3. **Array reallocation stress** — Grow/shrink mixer inputs while rendering

### Manual Testing
1. **Audio playback during rewire changes** — Should not crash or corrupt audio
2. **Position scrubbing** — Dragging playhead should not cause audio glitches
3. **Dynamic mixer resizing** — Adding/removing mixer inputs during playback

### Address Sanitizer
Build with `-fsanitize=thread` to detect any remaining races:
```bash
./rebuild.sh  # Includes ASan flags from CMakeLists.txt
./bin/smaragd.app/Contents/MacOS/smaragd  # Run with instrumentation
```

## Summary

All four derived component classes now follow the explicit _nolock() pattern from their base class. Critical operations that modify shared state are now protected by mutex locks, preventing use-after-free, buffer overflow, and audio corruption races.

The refactoring:
- ✅ Prevents component array reallocation races (twMixer, twRewire)
- ✅ Prevents latch deletion races (twRewire)
- ✅ Prevents position/offset races (twSampleReader, twWavInput)
- ✅ Maintains consistent lock scope (entire public method)
- ✅ Documents all `_nolock()` preconditions
- ✅ Builds successfully with no new warnings
- ✅ Follows established pattern from twComponent base class
