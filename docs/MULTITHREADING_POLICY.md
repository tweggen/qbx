# Smaragd Multithreading Policy

## Overview

Smaragd has two concurrent execution contexts:
- **UI Thread** (Qt main thread): handles user input, editing, rendering preview
- **Audio Thread** (real-time): CoreAudio/WASAPI/ALSA callback, produces samples

State shared between these threads causes race conditions that lead to crashes and visual glitches. This policy defines how to synchronize access to all shared mutable state.

## Thread Domains

### UI Thread
- Handles: User interaction, Qt signals/slots, preview rendering, parameter editing
- **Constraint:** Can block/wait (no real-time guarantee)
- **Allowed operations:** Mutex locks, condition variables, dynamic allocation

### Audio Thread
- Handles: Real-time audio production via `twSpeaker::readData()`
- **Constraint:** MUST NOT block; latency is strictly limited (typically ~10ms per buffer)
- **Allowed operations:** Read-only access, atomic loads, pre-allocated buffers

## Synchronization Strategy: Snapshot-Based with Immutable Reads

To avoid blocking the audio thread, we use **snapshot-based synchronization**:

1. **Audio thread reads a snapshot** of mutable state (copy/atomic load)
2. **UI thread modifies original** with mutex protection
3. **Audio thread never blocks** waiting for a lock

This approach:
- ✅ Audio thread has zero contention (reads only happen during setup/buffer boundaries)
- ✅ UI thread can modify freely (no blocked waits needed)
- ✅ Snapshot is cheap (copy or atomic ops for small data)

### Alternative Considered: Read-Write Locks
- ❌ Rejected: Would require audio thread to acquire read lock on every sample block
- ❌ Risk of priority inversion (audio thread waits for UI thread)

### Alternative Considered: Comprehensive Locking
- ❌ Rejected: Audio thread must not lock/block during `calcOutputTo()`

## Protected Data & Synchronization Mechanism

### SCut: Playback Window Parameters

**Shared mutable state:**
- `startOffset_` — source sample position where clip window begins
- `loopStart_` — loop segment start (relative to content, not used actively)
- `loopLength_` — loop segment length; 0 means no loop
- `cutDuration_` — timeline duration of clip (includes stretching)
- `looping_` — flag: whether reader is currently a twLoopReader
- `grainParams_` — time-stretch/pitch-shift parameters (struct with stretch, pitchCents, etc.)
- `reader_` — pointer to playback component (twSampleReader)
- `grain_` — pointer to grain processor (twGrainSource)
- `readerTried_` — flag: reader built or attempted

**Protection mechanism:** 
`std::mutex windowMutex_` + **snapshot pattern**

**Snapshot object:**
```cpp
struct SCutSnapshot {
    offset_t startOffset;
    length_t loopLength;
    length_t cutDuration;
    twGrainParams grainParams;
    bool looping;
    twSampleReader *reader;  // always valid: non-null or fallback to content_->getRootComponent()
};
```

**When to take snapshot:**
1. **On buffer boundary** (start of `twSpeaker::readData()` call)
2. **After UI modifies window** (rebuild reader on UI thread, don't rebuild on audio thread)

**Usage in audio thread:**
- Call `SCut::getSnapshot()` (lock-free if using atomics/precomputed)
- Use snapshot values for duration checks, seeking, reader selection
- Never call `setWindow()`, `setStartOffset()`, etc. from audio thread

**Usage in UI thread:**
- All modifications go through `setWindow()`, `setStartOffset()`, etc.
- These methods lock `windowMutex_` only during state write, then unlock before calling `rebuildReader()`
- `rebuildReader()` is UI-thread-only and non-blocking (no lock held)

### SObject: Volume Parameter

**Shared mutable state:**
- `volume_` — linear gain [0..2.0 or higher]

**Protection mechanism:**
`std::mutex volumeMutex_` + single atomic value (copy on read is cheap)

**Usage:**
- Audio thread: Call `getVolume()` which acquires lock, reads, unlocks (fast, ~100ns)
- UI thread: Call `setVolume(double)` which acquires lock, modifies, unlocks, invalidates preview

**Rationale:** Volume is a single `double`; one lock suffices. No snapshot needed.

### SPlainWave: File Handle

**Shared mutable state:**
- `twWavInput::file_` — QFile handle with seek/read position
- `twWavInput::dataStart_` — offset in file where sample data begins
- `twWavInput::playOffset_` — current playback position

**Protection mechanism:**
`std::mutex fileMutex_` (to be added)

**Usage:**
- Lock held during `seek()` and `read()` (operation must be atomic: seek then read)
- Both UI and audio threads acquire lock before touching file

**Rationale:** File seeks are not atomic; seek+read must be indivisible.

## Critical Sections (Must Be Atomic)

### In SCut::seekTo() — Audio Thread

**Current (UNSAFE):**
```cpp
int SCut::seekTo( offset_t off ) {
    ensureReader();
    if( reader_ ) return reader_->seekTo( looping_ ? off : off + startOffset_ );
    return content_->getSObject().seekTo( off+startOffset_ );
}
```

**Problem:** Reads `looping_` and `startOffset_` without synchronization. UI thread can modify between reads.

**Fixed version:**
```cpp
int SCut::seekTo( offset_t off ) {
    ensureReader();  // UI thread only, skip if already built
    SCutSnapshot snap = getSnapshot();  // Atomic: copy or atomic load
    if( snap.reader ) 
        return snap.reader->seekTo( snap.looping ? off : off + snap.startOffset );
    return content_->getSObject().seekTo( off + snap.startOffset );
}
```

### In SCut::getDuration() — Audio Thread & Serialization

**Current (UNSAFE):**
```cpp
length_t SCut::getDuration() const {
    return cutDuration_;  // Unprotected read
}
```

**Fixed version (read snapshot):**
```cpp
length_t SCut::getDuration() const {
    return getSnapshot().cutDuration;
}
```

### In Audio Playback Loop

**Pattern:** At buffer start, capture snapshot. Use only snapshot for all rendering decisions:
```cpp
SCutSnapshot snap = cut->getSnapshot();
// Now use snap.startOffset, snap.loopLength, snap.reader, etc.
// Safe: snapshot is immutable during this buffer
```

## Implementation Plan

### Phase 1: SCut Snapshot (CRITICAL)
1. Add `SCutSnapshot` struct to scut.h
2. Add `SCutSnapshot SCut::getSnapshot() const` method
3. Protect `getSnapshot()` with lock_guard on `windowMutex_`
4. Migrate all audio-thread reads to use `getSnapshot()`:
   - `seekTo()` 
   - `getDuration()` (if called from audio path)
   - `getRootComponent()` (update reader selection)

### Phase 2: File Access Synchronization
1. Add `std::mutex fileMutex_` to `twWavInput`
2. Protect all `file_.seek()` + `file_.read()` pairs with lock_guard

### Phase 3: Systematic Audit
1. Audit all methods called from audio thread (traverse from `twSpeaker::readData()` callees)
2. Mark them: `// AUDIO_THREAD_SAFE: reads snapshot` or `// AUDIO_THREAD_SAFE: no mutable state`
3. For any remaining unprotected reads: add snapshot or lock

### Phase 4: Documentation
1. Add thread-affinity annotations to all classes:
   ```cpp
   // Thread affinity: UI only / Audio only / MIXED (use snapshot/lock)
   ```
2. Add comments to all shared mutable members explaining their protection mechanism

## Race Condition Examples & Solutions

### Example 1: Window Parameter Inconsistency

**Scenario:** Audio thread is in `seekTo()`, UI thread modifies window.

**Before:**
```
Audio: reads looping_ = false
UI:    acquires lock, sets looping_ = true, sets startOffset_ = 100
Audio: reads startOffset_ = 100 (expected 0)
```

**After (with snapshot):**
```
Audio: takes snapshot { looping: false, startOffset: 0 }
UI:    acquires lock, modifies original startOffset_ = 100, looping_ = true
Audio: uses snapshot { looping: false, startOffset: 0 } — consistent!
```

### Example 2: File Seek/Read Race

**Scenario:** UI thread previews while audio plays.

**Before:**
```
Audio: file_.seek(1000)
UI:    file_.seek(500)
Audio: file_.read() — reads from pos 500, gets wrong data, crash
```

**After (with fileMutex):**
```
Audio: lock(fileMutex_) → file_.seek(1000) → file_.read() → unlock
UI:    lock(fileMutex_) → file_.seek(500) → file_.read() → unlock
// Operations are atomic; no interleaving
```

## Testing the Policy

1. **Playback + edit test:** Start playback, drag clip window while playing. Should not crash or flicker.
2. **Concurrent preview test:** Rapidly adjust volume slider + play. Should not crash.
3. **File access test:** Zoom waveform in/out while playing. Should not crash.

## Enforcement

- **Code review:** Every PR modifying SCut/SObject/SPlainWave must verify thread safety.
- **Comments:** Tag critical sections with `// RACE CONDITION FIX` and reference this policy.
- **Atomic operations:** Prefer atomic<T> over locks for simple scalars when safe.

## References

- `smaragd/main/include/scut.h` — Current implementation (needs snapshot)
- `smaragd/main/include/sobject.h` — Volume mutex (adequate)
- `smaragd/main/include/splainwave.h` — File access (needs fileMutex)
- `plan/STATE.md` — Implementation log
