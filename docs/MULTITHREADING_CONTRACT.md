# Smaragd Multithreading Contract

## Purpose

This document defines the legal operations and synchronization boundaries for Smaragd's UI and audio threads. Every function must adhere to this contract or explicitly document deviations.

## Thread Domains

### UI Thread
- Entry points: Qt slots, button handlers, mouse events, menu actions
- Constraint: Can block, allocate, deallocate freely
- Responsibilities: All user-facing state changes, preview rendering, playback control

### Audio Thread
- Entry point: Real-time audio callback (twSpeaker::readData)
- Constraint: MUST NOT block or wait; latency ≤ ~10ms per buffer
- Responsibilities: Produce audio samples, update play position

## The Contract: Three Core Rules

### Rule 1: Snapshot Pattern for Window Parameters

**Scope:** All access to SCut window parameters during playback
- `startOffset_`, `loopLength_`, `cutDuration_`, `grainParams_`, `looping_`, `reader_`

**How it works:**
```
Audio Thread (per buffer):
  1. Call snap = cut->getSnapshot()  [acquires lock, copies all params, releases]
  2. Use snap.startOffset, snap.loopLength, etc. throughout buffer
  3. Never directly access cut->startOffset_ etc.

UI Thread (when parameter changes):
  1. Acquire windowMutex_
  2. Modify all related parameters (startOffset, loopLength, etc.)
  3. Take snapshot: snap = getSnapshot()  [no additional lock needed, already held]
  4. Release windowMutex_
  5. Pass snap to rebuildReader(snap)  [rebuild uses snapshot, not raw members]
```

**Guarantees:**
- Audio thread always reads a consistent snapshot
- UI thread modifications are atomic with snapshot capture
- rebuildReader never races with snapshots (reads only the passed snapshot)

### Rule 2: Reader Stability During Playback

**Problem:** Audio thread holds pointer to `reader_` from snapshot, but UI thread can delete it via rebuildReader()

**Solution:** Generation Counter (Optimistic Consistency)

```cpp
struct SCutSnapshot {
    // ... all window params ...
    twSampleReader *reader;
    int readerGeneration;  // NEW: increments when reader is replaced
};

// Audio thread logic:
SCutSnapshot snap = cut->getSnapshot();
// Use snap.reader and snap.readerGeneration

// Later in same buffer:
int currentGen = cut->getReaderGeneration();
if (currentGen != snap.readerGeneration) {
    // Reader was rebuilt; snapshot is stale, re-snapshot
    snap = cut->getSnapshot();
}
// Continue with current snap

// UI thread logic:
void rebuildReader(const SCutSnapshot &snap) {
    // ... build new reader ...
    {
        std::lock_guard<std::mutex> lock(windowMutex_);
        reader_ = newReader;
        readerGeneration_++;  // Increment on replacement
    }
}
```

**Guarantees:**
- Audio thread never uses a deleted reader pointer
- If reader is rebuilt mid-buffer, audio detects it and re-snapshots
- No blocking of audio thread (generation check is atomic read)
- Live editing allowed: UI can rebuild anytime

**Constraints:**
- Reader generation must be atomic<int> or protected by lock
- Audio thread must check generation periodically (after long-running operations)

### Rule 3: Volume Parameter (Single Scalar)

**Scope:** `SObject::volume_`

**How it works:**
```
Audio Thread:
  1. Acquire volumeMutex_
  2. Read volume_
  3. Release volumeMutex_
  (Fast: ~100ns overhead, acceptable during buffer processing)

UI Thread:
  1. Acquire volumeMutex_
  2. Modify volume_
  3. Release volumeMutex_
  4. Invalidate preview cache
```

**Guarantees:**
- No torn reads of volume value
- Preview cache coherency

## Synchronization Points

### At Buffer Start (Audio Thread)
```cpp
void twSpeaker::readData(...) {
    // Synchronization point: take snapshot of all needed SCuts
    for (SCut *cut : activePlayingCuts) {
        SCutSnapshot snap = cut->getSnapshot();
        // Work with snap throughout this buffer
    }
}
```

### On Parameter Change (UI Thread)
```cpp
void SCut::setWindow(...) {
    // Synchronization point: atomic capture
    {
        std::lock_guard<std::mutex> lock(windowMutex_);
        // Modify all related members
        startOffset_ = ...;
        cutDuration_ = ...;
        loopLength_ = ...;
        // Capture while holding lock
        SCutSnapshot snap = getSnapshot();
    }
    // Rebuild with snapshot (safe: rebuildReader reads only snap, not members)
    rebuildReader(snap);
}
```

### On Reader Rebuild (UI Thread)
```cpp
void SCut::rebuildReader(const SCutSnapshot &snap) {
    // This function is safe to call anytime, even during playback
    // Because it reads only the passed snapshot, not any member variables
    
    // Build new reader...
    twSampleReader *newReader = ...;
    
    // Atomic swap and generation increment
    {
        std::lock_guard<std::mutex> lock(windowMutex_);
        if (reader_) delete reader_;
        reader_ = newReader;
        readerGeneration_++;  // Signal to audio that reader changed
    }
}
```

## Forbidden Operations

❌ **UI Thread:**
- Cannot call audio-only functions from non-UI context
- Cannot assume reader pointer stability across lock releases
- Cannot modify window parameters without lock

❌ **Audio Thread:**
- Cannot hold locks (ever)
- Cannot allocate/deallocate memory
- Cannot call UI-thread-only functions
- Cannot directly read `startOffset_`, `loopLength_`, `cutDuration_`, `reader_`, etc.
  - Must use snapshot via getSnapshot()
- Cannot assume reader pointer is stable
  - Must check readerGeneration before using reader across significant operations

❌ **Both Threads:**
- Cannot access mutable state without documented synchronization
- Cannot assume pointer stability without refcounting/generation tracking

## Implementation Requirements

### For SCut (Window Parameters)

**Members to protect:**
```cpp
mutable std::mutex windowMutex_;
offset_t startOffset_;
length_t loopLength_;
length_t cutDuration_;
twGrainParams grainParams_;
bool looping_;
twSampleReader *reader_;
std::atomic<int> readerGeneration_;  // NEW
```

**Methods:**
```cpp
// Public
SCutSnapshot getSnapshot() const;
int getReaderGeneration() const;  // NEW: atomic read
void rebuildReader(const SCutSnapshot &snap);

// Private
// getSnapshot() implementation:
// 1. Acquire lock
// 2. Copy all window params into snapshot
// 3. Release lock
// 4. Return snapshot
```

### For SObject (Volume)

**No changes needed** — already protected with volumeMutex_

### For twWavInput/twSampleSource

**Status:** ✅ Already thread-safe
- Files loaded entirely into RAM at construction
- read() is lock-free memcpy
- No file I/O in realtime path

## Testing Requirements

### Must Pass
1. **Playback + edit test:** Start playback, drag clip window boundaries while playing
   - Expected: smooth playback, no glitches, no crashes
2. **Rapid parameter changes:** Change stretch/pitch/volume while playing
   - Expected: changes visible on next buffer, no corruption
3. **Reader changes:** Modify grain params while playing (triggers rebuildReader)
   - Expected: rebuild completes safely, audio continues uninterrupted
4. **Preview during playback:** Zoom/pan waveform while playing
   - Expected: preview refreshes correctly
5. **Stress test:** Rapid edits of multiple cuts while playing all of them
   - Expected: no crashes, stable playback

### Must NOT Happen
- Use-after-free crashes (reader deleted while in use)
- Waveform preview corruption (inconsistent parameter reads)
- Audio glitches or pops (blocking in audio thread)
- Memory leaks from readers or snapshots

## Enforcement

### Code Review Checklist
- [ ] Audio-thread callees never hold locks
- [ ] Window parameter reads always use snapshot or are protected by lock
- [ ] rebuildReader only reads passed snapshot, not members
- [ ] Reader pointers checked for generation before long operations
- [ ] Audio thread never assumes pointer stability

### Documentation Requirements
- [ ] Every class has thread-affinity comment
- [ ] Every function used from audio thread marked `// AUDIO_THREAD_SAFE`
- [ ] All synchronization mechanisms documented inline

### Automated Verification (Future)
- Static analysis: detect direct member reads outside locks
- Runtime assertions: check getSnapshot() is called from audio path
- Mutex poisoning: detect if audio thread ever acquires windowMutex_

## Example: Safe Playback + Edit Flow

```
Time  Audio Thread                      UI Thread
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
t=0   [Buffer start]
      snap = cut->getSnapshot()
      [Uses snap throughout buffer]

t=0.5                                   [User drags clip window]
                                        lock(windowMutex_)
                                        startOffset_ = 500
                                        cutDuration_ = 2000
                                        snap = getSnapshot()
                                        unlock
                                        rebuildReader(snap)

t=1.0 [Buffer end]
      gen = cut->getReaderGeneration()
      if (gen != snap.readerGeneration) {
          snap = cut->getSnapshot()  // ← Detects change, re-snapshots
      }
      [Start next buffer with current snap]

t=1.0 [Next buffer start]
      snap = cut->getSnapshot()
      [Uses new snap with updated reader]
```

## Rationale

This contract provides:
- **Determinism:** Audio thread sees consistent snapshots
- **Responsiveness:** UI changes visible on next buffer, no waiting
- **Safety:** No use-after-free, no corruption
- **Performance:** Zero audio-thread blocking
- **Simplicity:** Clear rules, easy to verify

## Migration Path

**Phase 1 (DONE):** Snapshot for window parameters, rebuildReader(snap)
**Phase 2 (TODO):** Add readerGeneration_ counter and generation checks
**Phase 3 (TODO):** Audit all audio-path functions for contract compliance
**Phase 4 (TODO):** Add enforcement annotations and tests
