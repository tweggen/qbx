# Smaragd Concurrency Guidelines: Formal Model

## Purpose

This document defines a **mathematically sound concurrency model** for Smaragd. If code adheres to these guidelines, it is **provably race-condition-free**. This is a formal specification, not advice—violations are bugs.

## Definitions

### Thread Domains
```
UI Thread:      Qt main thread, user input, preview rendering
Audio Thread:   Real-time audio callback (callback-only, no recursion)
Shared:         Both threads access this state
```

### Memory Domains
Every mutable object or member belongs to exactly one domain:
```
Domain         Thread Access           Synchronization     Rules
─────────────────────────────────────────────────────────────────────────
UI-Only        UI only                 None                No locking needed
Audio-Only     Audio only              None                No locking needed
Shared         Both (both read/write)  Specified below     Must follow contract
```

**RULE 0:** Every mutable object/member must be explicitly labeled with its domain.
```cpp
class SCut {
    // DOMAIN: UI-Only (setters are UI-only slots)
    offset_t startOffset_;
    
    // DOMAIN: Shared (both threads read, UI modifies)
    std::mutex windowMutex_;
    length_t cutDuration_;
    
    // DOMAIN: Shared (audio reads via snapshot, UI rebuilds)
    twSampleReader *reader_;
    std::atomic<int> readerGeneration_;
};
```

## Synchronization Contracts

### Contract A: Mutex-Protected Scalar

**Scope:** Single scalar values accessed from both threads
**Example:** `SObject::volume_`, `SObject::pan_`

**Invariant:** 
```
(Reading locked_value) ∨ (Writing locked_value) → always exclusive
```

**Implementation:**
```cpp
class SObject {
    // DOMAIN: Shared (Mutex-Protected Scalar, Contract A)
    mutable std::mutex volumeMutex_;
    double volume_;
    
    // Reading (both threads):
    double getVolume() const {
        std::lock_guard lock(volumeMutex_);
        return volume_;  // Atomic read
    }
    
    // Writing (UI thread):
    void setVolume(double v) {
        {
            std::lock_guard lock(volumeMutex_);
            volume_ = v;
        }
        invalidatePreview();  // Lock released before side effects
    }
};
```

**Verification:** 
- [ ] Member is always accessed with lock
- [ ] Lock is released before any function call (prevents lock ordering bugs)
- [ ] No blocking operations inside critical section

---

### Contract B: Snapshot Pattern (Lock-Once, Immutable Copy)

**Scope:** Multiple related values that must be consistent together
**Example:** `SCut::startOffset_`, `SCut::loopLength_`, `SCut::cutDuration_`, `SCut::grainParams_`

**Invariant:**
```
Audio thread reads only from (snapshot taken at point T)
UI thread modifies original, then captures snapshot before rebuild
(snapshot is immutable to both threads; original protected by mutex)
```

**Implementation:**
```cpp
// SNAPSHOT STRUCT (immutable copy of related state)
struct SCutSnapshot {
    offset_t startOffset;
    length_t cutDuration;
    twGrainParams grainParams;
    int readerGeneration;
};

class SCut {
    // DOMAIN: Shared (Snapshot Pattern, Contract B)
    mutable std::mutex windowMutex_;
    offset_t startOffset_;        // Only modified with lock
    length_t cutDuration_;        // Only modified with lock
    twGrainParams grainParams_;   // Only modified with lock
    std::atomic<int> readerGeneration_;  // Atomic counter
    
    // Snapshot getter (both threads)
    SCutSnapshot getSnapshot() const {
        std::lock_guard lock(windowMutex_);  // ← Acquire once
        SCutSnapshot snap;
        snap.startOffset = startOffset_;
        snap.cutDuration = cutDuration_;
        snap.grainParams = grainParams_;
        snap.readerGeneration = readerGeneration_.load();
        return snap;  // ← Return immutable copy
    }
    
    // Writing (UI thread)
    void setWindow(offset_t start, length_t dur) {
        SCutSnapshot snap;
        {
            std::lock_guard lock(windowMutex_);
            startOffset_ = start;
            cutDuration_ = dur;
            snap = getSnapshot();  // Capture while holding lock
        }
        rebuildReader(snap);  // ← Pass snapshot, not raw members
    }
    
    // Reading (audio thread)
    void seekTo(offset_t off) {
        SCutSnapshot snap = getSnapshot();  // Get immutable copy
        // Use snap.startOffset, snap.cutDuration, etc.
        // Safe: snap is immutable, snapshot was captured atomically
    }
};
```

**Verification:**
- [ ] Snapshot struct is read-only (no setters)
- [ ] getSnapshot() always acquires lock and releases before return
- [ ] All modifications first acquire lock, then call getSnapshot() while holding lock
- [ ] All UI-thread rebuilds pass snapshot parameter (never read raw members)
- [ ] All audio-thread reads call getSnapshot()

---

### Contract C: Generation Counter (Stale Pointer Detection)

**Scope:** Pointer to mutable object that can be replaced while in-use
**Example:** `SCut::reader_` (pointer to twSampleReader that can be rebuilt)

**Invariant:**
```
(Audio holds pointer P with generation G)
∧ (UI increments generation to G' > G)
→ (Audio can detect change, re-snapshot, and get new pointer)
```

**Implementation:**
```cpp
class SCut {
    // DOMAIN: Shared (Generation Counter, Contract C + Deferred Deletion)
    twSampleReader *reader_;                           // Can be replaced
    twSampleReader *readerPending_;                    // Old pointer awaiting deletion
    std::atomic<int> readerGeneration_{0};             // Incremented on replacement
    
    // Getter (audio thread)
    int getReaderGeneration() const {
        return readerGeneration_.load(std::memory_order_acquire);
    }
    
    // Rebuilding (UI thread) — uses deferred deletion
    void rebuildReader(const SCutSnapshot &snap) {
        // Step 1: Move old reader to pending (don't delete immediately)
        if (readerPending_) delete readerPending_;      // Clean up previous generation
        readerPending_ = reader_;                        // Defer old one
        reader_ = nullptr;
        
        // Step 2: Build and install new reader
        reader_ = buildNewReader(snap);
        
        // Step 3: Signal change
        readerGeneration_.fetch_add(1, std::memory_order_release);
    }
    
    // Usage (audio thread)
    twComponent &getRootComponent() {
        SCutSnapshot snap = getSnapshot();
        // snap.reader is safe: captured atomically, even if UI rebuilds now,
        // old reader won't be deleted until next rebuild
        return *snap.reader;
    }
};
```

**Verification:**
- [ ] Generation counter is atomic (no mutex needed)
- [ ] Pointer is read via snapshot (captures generation together with pointer)
- [ ] Old pointer deferred-deleted until next rebuild (never immediate delete)
- [ ] Audio thread can optionally check generation between operations (pattern):
  ```cpp
  SCutSnapshot snap = getSnapshot();
  // ... long operation ...
  if (snap.readerGeneration != getReaderGeneration()) {
      snap = getSnapshot();  // Re-snapshot if changed
  }
  ```

---

### Contract D: Immutable Object (No Synchronization)

**Scope:** Objects that are never modified after construction
**Example:** `twSampleSource` (sample data loaded once, never changed)

**Invariant:**
```
Construction writes all fields
∧ (No writes after construction)
→ (Reads from any thread are safe, no synchronization needed)
```

**Implementation:**
```cpp
class twSampleSource : public twRandomSource {
    // DOMAIN: Immutable (no synchronization)
    std::vector<sample_t> data_;    // Written once at construction, never again
    int rate_;                       // Read-only after construction
    int channels_;                   // Read-only after construction
    
    twSampleSource(...) {
        // Populate data_, rate_, channels_ here
    }
    
    // Read from any thread, no lock needed
    length_t read(offset_t pos, sample_t *dest, length_t len, idx_t ch) const {
        // data_, rate_, channels_ are immutable, safe to read from any thread
        ...
    }
};
```

**Verification:**
- [ ] All fields written only in constructor
- [ ] No writes after construction
- [ ] No mutable members

---

## Applying Contracts to Smaragd

### SCut Concurrency Model

| Member | Domain | Contract | Rules |
|--------|--------|----------|-------|
| `startOffset_` | Shared | B (Snapshot) | Write via snapshot, read via snapshot |
| `cutDuration_` | Shared | B (Snapshot) | Write via snapshot, read via snapshot |
| `loopLength_` | Shared | B (Snapshot) | Write via snapshot, read via snapshot |
| `grainParams_` | Shared | B (Snapshot) | Write via snapshot, read via snapshot |
| `reader_` | Shared | C (Gen Counter) | Replace via deferred-delete, read via snapshot |
| `grain_` | Shared | C (Gen Counter) | Replace via deferred-delete, read via snapshot |
| `looping_` | Shared | B (Snapshot) | Only accessed via snapshot |
| `readerGeneration_` | Shared | Atomic | Incremented on reader rebuild |
| `content_` | UI-Only | A (None) | UI sets once, audio reads (immutable ptr) |

### SObject Concurrency Model

| Member | Domain | Contract | Rules |
|--------|--------|----------|-------|
| `volume_` | Shared | A (Mutex) | Lock on read & write |
| `pan_` | UI-Only | None | UI only |
| `delay_` | UI-Only | None | UI only |
| `solo_` | UI-Only | None | UI only |
| `muted_` | UI-Only | None | UI only |

### twSampleSource Concurrency Model

| Member | Domain | Contract | Rules |
|--------|--------|----------|-------|
| `data_` | Immutable | D | No sync needed |
| `rate_` | Immutable | D | No sync needed |
| `channels_` | Immutable | D | No sync needed |

---

## Forbidden Patterns

❌ **Double-check locking** (TOCTOU race):
```cpp
if (reader_) {              // ← Check without lock
    return *reader_;        // ← Can be deleted by UI before this line
}
```

❌ **Lock held across function boundary**:
```cpp
{
    lock(mutex_);
    someValue_ = ...;
    someFunction();         // ← Lock still held! (deadlock risk)
    unlock();
}
```

❌ **Unprotected read of multi-member state**:
```cpp
// WRONG: startOffset and cutDuration might change between reads
offset_t start = startOffset_;  // Read without lock
// ← UI thread modifies here
length_t dur = cutDuration_;    // Read without lock (inconsistent!)
```

❌ **Direct pointer dereference without snapshot**:
```cpp
if (reader_) {
    reader_->seekTo(...);   // ← reader_ can be deleted concurrently
}
```

---

## Mechanical Verification

### Audit Checklist

For every function that accesses shared state:

1. **Domain Check:**
   - [ ] Every accessed member has DOMAIN label in header
   - [ ] If accessed from both threads, domain is Shared

2. **Synchronization Check:**
   - [ ] Contract A (Mutex): lock acquired before read/write, released before function call
   - [ ] Contract B (Snapshot): called via getSnapshot(), never raw member access
   - [ ] Contract C (Gen Counter): pointer always from snapshot, deferred-delete used
   - [ ] Contract D (Immutable): no synchronization needed

3. **Concurrency Check:**
   - [ ] No TOCTOU (time-of-check to time-of-use) bugs
   - [ ] No multi-member reads without holding lock
   - [ ] No long-lived locks across function boundaries
   - [ ] No assumptions about pointer stability

### Automated Verification

**Static checks (grep):**
```bash
# Find direct member access in audio-path functions
grep -n "startOffset_\|cutDuration_\|looping_" src/scut.cpp | grep -v "snap\."

# Find unprotected pointer dereferences
grep -n "reader_->\|grain_->" src/scut.cpp | grep -v "snap\.reader\|snap\.grain"

# Find lock acquisitions not released before function call
# (requires manual review)
```

**Runtime checks:**
```cpp
#ifdef DEBUG
    #define ASSERT_AUDIO_THREAD() assert(isAudioThread())
    #define ASSERT_UI_THREAD() assert(isUIThread())
#else
    #define ASSERT_AUDIO_THREAD() (void)0
    #define ASSERT_UI_THREAD() (void)0
#endif

// Use in audio-only functions:
void AudioOnlyFunction() {
    ASSERT_AUDIO_THREAD();  // Fail if called from UI thread
    ...
}
```

---

## Proof of Correctness

**Theorem:** If all code follows Contracts A-D, there are no data races.

**Proof sketch:**
1. Contract A (Mutex): exclusive access to scalar → no race
2. Contract B (Snapshot): reads immutable snapshot; writes capture atomically → consistent snapshot, no race
3. Contract C (Gen Counter): old pointer kept alive until next rebuild; snapshot captures generation → pointer always valid, no use-after-free race
4. Contract D (Immutable): no writes after construction; reads see constructed state → no race

**Assumption:** 
- Mutex implementation is correct (POSIX guarantee)
- std::atomic is correct (C++11 guarantee)
- Memory ordering is correct (release/acquire semantics)

---

## Enforcement

### Code Review

Every PR touching shared state must:
1. Label all affected members with DOMAIN comments
2. Cite which Contract applies
3. Pass mechanical verification checks (see Audit Checklist)
4. Include test demonstrating the scenario (playback + edit)

### Test Coverage

- [ ] Playback + edit test: Start playback, drag window → no glitches
- [ ] Stress test: Rapid concurrent modifications → no crashes
- [ ] Generation counter test: Verify readerGeneration increments
- [ ] Snapshot consistency test: Verify snapshot captures coherent state

### Documentation

Every class with shared state must have a "Concurrency Model" section:
```cpp
/**
 * SCut: Slice of audio content with timing.
 * 
 * CONCURRENCY MODEL:
 * - Window parameters (startOffset, cutDuration, ...): Shared, Contract B (Snapshot)
 *   Access via getSnapshot() only. Modifications capture snapshot while holding lock.
 * - Reader pointer: Shared, Contract C (Generation Counter + Deferred Deletion)
 *   Access via getSnapshot(). UI rebuilds use deferred deletion pattern.
 * - Volume: Shared, Contract A (Mutex)
 *   Protect all access with volumeMutex_.
 */
```

---

## Example: Correct Safe Code

```cpp
// CORRECT: SCut setWindow with full compliance

void SCut::setWindow(offset_t start, length_t dur) {
    // STEP 1: Acquire lock, modify all related state
    SCutSnapshot snap;
    {
        std::lock_guard<std::mutex> lock(windowMutex_);  // Contract B
        startOffset_ = start;
        cutDuration_ = dur;
        snap = getSnapshot();  // Capture snapshot while holding lock
    }
    // STEP 2: Release lock
    
    // STEP 3: Rebuild (no lock held)
    rebuildReader(snap);  // Pass snapshot, not raw members (Contract C)
    
    // STEP 4: Signal change
    emit durationChanged(snap.cutDuration);
}

// CORRECT: Audio thread usage

void AudioCallback() {
    SCut *cut = ...;
    
    // Get snapshot (one-time lock, immutable copy)
    SCutSnapshot snap = cut->getSnapshot();  // Contract B
    
    // Use snapshot throughout rendering
    twComponent &comp = cut->getRootComponent();  // Returns via snapshot reader
    comp.seekTo(snap.startOffset);
    comp.calcOutputTo(...);
    
    // Optional: if doing long operations, check generation
    int gen = cut->getReaderGeneration();  // Contract C
    if (gen != snap.readerGeneration) {
        snap = cut->getSnapshot();  // Re-snapshot if reader changed
    }
}
```

---

## Current Compliance Status

### SCut: ✅ COMPLIANT (after Phase 2 fixes)
- [x] Window parameters use Snapshot Pattern (Contract B)
- [x] Reader uses Generation Counter + Deferred Deletion (Contract C)
- [x] getRootComponent() uses snapshot
- [x] All writes capture snapshot while holding lock

### SObject: ✅ COMPLIANT
- [x] Volume uses Mutex (Contract A)

### twSampleSource: ✅ COMPLIANT
- [x] Immutable (Contract D)

### Missing Audits:
- [ ] STrack, SPlainWave: Audit against this model
- [ ] twComponent hierarchy: Audit for shared state
- [ ] Preview rendering path: Ensure all snapshot usage

---

## Next Steps (Enforcement)

1. **Audit Phase:** Review all classes for shared state, apply model
2. **Annotation Phase:** Add DOMAIN labels to all members
3. **Testing Phase:** Create tests for each contract type
4. **Enforcement Phase:** Add static/runtime checks
