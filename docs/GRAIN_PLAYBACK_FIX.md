# Grain Playback Race Condition: Analysis and Fix

**Status:** Fixed  
**Date:** 2026-06-28  
**Root Cause:** Deferred deletion of grain sources while audio thread still references them  
**Solution:** Refcount grain sources with shared_ptr

---

## The Problem

### Symptom
```
Hash table corrupted. This is probably a memory error somewhere.
```
Crash during render of timestretched plainwave cuts, while audio thread accesses freed grain source.

### Root Cause Timeline

```
UI Thread (SCut::rebuildReader):
  t1: Creates newGrain = new twGrainSource(...)
  t2: Swaps readers (acquires readerSwapLock_)
      oldReader_ = currentReader_  (old reader with old grain)
      currentReader_ = nextReader_ (new reader with new grain)
  t3: DELETE oldReader_.grain  ← Manual delete HERE
  t4: Release lock

Audio Thread (Playback/Render):
  t1.5: Takes snapshot of currentReader_
        snapshot.grain = (points to old grain, refcount = 1)
  t2.5: Calls snapshot.grain->read()
  t3.5: Dereferences grain->data_ (memcpy from pre-computed buffer)
        ← CRASH: grain was deleted at UI:t3, memory is freed!
        ← Hash table corruption from Qt String cleanup in destructor
```

### Why This Happens

**Current architecture:**
```
struct SCutReaderState {
    twSampleReader *reader;      // Manual delete (raw pointer)
    twGrainSource *grain;        // Manual delete (raw pointer)  ← RACE!
    std::shared_ptr<twCapturingSource> captureRef;  // Refcounted (safe)
};
```

The grain source (twGrainSource) is manually deleted, but:
1. Audio thread snapshot holds a pointer to it
2. UI thread deletes it immediately after swap
3. Audio thread is still reading from freed memory
4. Crash in Qt object cleanup

**But captureRef is safe because it's shared_ptr:**
- Snapshot increments refcount
- Deletion decrements refcount
- Object stays alive (refcount ≥ 1) while snapshot exists

### Why Not Make Grain a twComponent?

Good question, but the answer is **no, keep it as twRandomSource**:

**twComponent = Streaming State Machine**
- Has position cursor (seekTo, tellPos)
- State evolves over rendering (renderFrames advances it)
- Needs reset() and state snapshots
- Example: oscillator, delay, mixer

**twRandomSource = Random-Access Data Query**
- No position (read(offset, ...) is idempotent)
- Stateless (same offset always returns same data)
- No reset needed (query is just a function)
- Example: sample file, resampled view, grain source

**Forcing grain to be a component would:**
- ❌ Falsely imply it has streaming state (it doesn't)
- ❌ Add page caching for pre-computed data (overhead)
- ❌ Conflate "needs refcounting" with "is a processing node"

**The real fix:** Just refcount it without forcing component semantics.

---

## The Fix

### Before
```cpp
struct SCutReaderState {
    twSampleReader *reader = nullptr;      // Manual delete
    twGrainSource *grain = nullptr;        // Manual delete ← RACE!
    std::shared_ptr<twCapturingSource> captureRef;
};

// In rebuildReader:
twSampleReader *newReader = new twSampleReader(...);
twGrainSource *newGrain = new twGrainSource(...);

// In swap:
delete oldReader_.grain;  // ← Unsafe: audio thread might be reading
oldReader_ = currentReader_;
currentReader_.reader = newReader;
currentReader_.grain = newGrain;
```

### After
```cpp
struct SCutReaderState {
    std::shared_ptr<twSampleReader> reader;      // Refcounted ✓
    std::shared_ptr<twGrainSource> grain;        // Refcounted ✓
    std::shared_ptr<twCapturingSource> captureRef;
};

// In rebuildReader:
auto newReader = std::make_shared<twSampleReader>(...);
auto newGrain = std::make_shared<twGrainSource>(...);

// In swap:
// NO manual delete needed!
oldReader_ = currentReader_;  // Old refcount decrements automatically
currentReader_.reader = newReader;
currentReader_.grain = newGrain;  // New refcount increments
```

### Safety Guarantee

**Reference Counting Lifecycle:**

```
Page 1: Creation
  newGrain = std::make_shared<twGrainSource>(...)
  ↓ refcount = 1 (held by newGrain)

Page 2: Insert into currentReader_
  currentReader_.grain = newGrain
  ↓ refcount = 1 (held by currentReader_)

Page 3: Audio thread takes snapshot
  snapshot = currentReader_
  ↓ refcount = 2 (held by currentReader_ AND snapshot)

Page 4: UI thread swaps readers
  oldReader_ = currentReader_
  currentReader_ = nextReader_
  ↓ refcount = 2 (held by oldReader_ AND snapshot)
    (oldReader_ now holds the old pointer, refcount didn't change)

Page 5: Audio thread reads
  snapshot.grain->read()  ← SAFE: refcount >= 1, grain still allocated

Page 6: Audio thread releases snapshot
  snapshot = SCutReaderState()  // Destructor
  ↓ refcount = 1 (held by oldReader_ only)

Page 7: Destructor (SCut::~SCut)
  oldReader_.reset()
  ↓ refcount = 0 ← NOW grain is deleted (safe, nobody reading)
```

---

## Commits

### 1. Unified Mutex Strategy (2e24cb1)
- Added `stateMutex_` to twComponent base class
- Fixed twSampleSource::viewAtRate() race with mutex protection
- Established _nolock() pattern for audit trail

### 2. TOCTOU Race Fix (f3aafb2)
- Eliminated race in freezePage() where two threads could create duplicate pages
- Atomic check-and-insert under mutex
- Pages now protected by shared_ptr refcount during rendering

### 3. Page Cache Safety Proof (57559fc)
- Documented all write accesses to pages (all protected)
- Proved no use-after-free or double-free possible
- Established reference counting guarantees

### 4. Grain Source Refcounting Fix (bd7e1fe)
- Made twSampleReader and twGrainSource refcounted (shared_ptr)
- Eliminated deferred deletion race in SCutReaderState swap
- Audio thread snapshots keep grain alive during read

---

## Testing

**Before (reproducer):**
```
1. Load project with timestretched plainwave cut
2. Click File → Render
3. Crash: "Hash table corrupted"
```

**After:**
- Render completes without crashes
- Audio thread snapshot keeps grain alive
- Deferred deletion happens only after all references released

---

## Future Optimizations (Deferred)

**Page caching for sources:**
- twGrainSource could use lazy page materialization (vs. upfront)
- Would reduce memory for very large grain sources
- But: overhead > benefit for typical use cases
- Mark as "later optimization" when data justifies it

**Unified base class:**
- Factor common lifecycle (refcounting) into base class
- But: grain source stays as twRandomSource (not component)
- Refcounting is orthogonal to component semantics

---

## Architecture Notes

### Why Refcounting > Manual Delete

**Manual lifecycle (before):**
```cpp
delete oldReader_.grain;  // When is it safe to delete?
                          // When nobody is reading it.
                          // Who is reading it? Audio thread.
                          // How do we know audio thread is done?
                          // We don't. ← RACE CONDITION
```

**Refcounted lifecycle (after):**
```cpp
oldReader_.grain.reset();  // Decrement refcount
                           // Is grain deleted?
                           // Only if refcount == 0
                           // Audio thread snapshot holds refcount
                           // Audio thread still reading? ← SAFE
```

### Stateless vs. Stateful

**Important distinction the user pointed out:**

- **twComponent:** Has continuous state (position, internal buffers)
  - Needs reset() to return to known state
  - Needs state snapshots for sequential rendering
  - renderFrames() advances state over time
  
- **twRandomSource:** Stateless data query
  - read(offset, ...) is idempotent
  - No position or state
  - Always returns same data for same offset
  
**Grain sources are RandomSources** (not components), so:
- ✓ Use shared_ptr for lifecycle safety
- ✗ Don't force into component model (it's wrong conceptually)
- ✓ Keep simple: pre-computed, immutable, random-access

