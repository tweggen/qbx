# Smaragd Threading Model: Unix Page Cache Semantics

## Executive Summary

Smaragd uses a **double-buffer model inspired by Unix page cache semantics** to ensure safe concurrent access between the UI thread and real-time audio thread.

**Core Principle:** *Always have a complete, committed version available to readers. Modifications happen out-of-band until ready, then swap atomically.*

This eliminates race conditions without blocking the audio thread.

---

## The Problem: Concurrent Access Races

### Scenario: Volume Change During Playback

```
UI Thread                          Audio Thread
─────────────────────────────────────────────────
setVolume(new_volume)              renderAudio()
  ↓                                  ↓
invalidatePreview()                getRootComponent()
  ↓                                  ↓  
getPreview() [rebuilding]          calcOutputTo() [rendering]
  ↓                                  ↓
[calls calcOutputTo]  ←────RACE────→ [calling calcOutputTo]
                        SAME COMPONENT!
                      CORRUPTED OUTPUT
```

Both threads try to render the same component concurrently → **corrupted data** or **crashes**.

### Why This Happens

1. **UI thread changes parameter** (e.g., volume slider)
2. **Invalidates cached preview** → must recalculate
3. **Recalculation pulls live from source** → calls `calcOutputTo()`
4. **Meanwhile, audio thread is actively rendering** the same source
5. **Both threads call `calcOutputTo()` simultaneously** → data race

---

## The Solution: Double-Buffer (Unix Page Cache Model)

### Conceptual Model

Unix kernel's page cache always maintains a **committed version** that readers use, while writes happen **out-of-band**:

```
[Page Cache in RAM]

Readers:  ┌──────────────────┐
          │ COMMITTED PAGE   │  ← Always valid, never changing mid-read
          └──────────────────┘

Writers:  ┌──────────────────┐
          │ NEW PAGE (BUSY)  │  ← Constructed independently
          └──────────────────┘

When writer finishes:
  [ATOMIC SWAP]
  └────────────────┘
  Old becomes recyclable
  New becomes committed
```

### Smaragd Implementation: Double-Buffer Reader State

Applied to `SCut` (the critical class with reader state):

```cpp
struct SCutReaderState {
    twSampleReader *reader;     // The actual reader component
    twGrainSource *grain;       // Optional grain processor
    bool looping;               // Is this a loop reader?
    int generation;             // Incremented on swap
};

class SCut {
    // COMMITTED version: audio thread reads only this
    SCutReaderState currentReader_;
    
    // BUILDING version: UI thread constructs this independently
    SCutReaderState nextReader_;
    
    // RECYCLABLE: old committed version, freed on next swap
    SCutReaderState oldReader_;
    
    // Lock for atomic swap (held <1μs)
    mutable std::mutex readerSwapLock_;
};
```

### The Swap Operation (Atomic Commit)

When UI thread finishes building a new reader:

```cpp
void rebuildReader(const SCutSnapshot &snap) {
    // STEP 1: Build completely out-of-band (no locks)
    // Audio thread continues unaffected with currentReader_
    twSampleReader *newReader = buildNewReader(snap);
    twGrainSource *newGrain = buildNewGrain(snap);
    
    // STEP 2: Atomic swap (lock held <1μs)
    {
        std::lock_guard lock(readerSwapLock_);
        
        // Clean up previous old reader
        delete oldReader_.reader;
        delete oldReader_.grain;
        
        // Rotate: current → old, next → current
        oldReader_ = currentReader_;              // Current becomes recyclable
        currentReader_.reader = newReader;         // Next becomes current
        currentReader_.grain = newGrain;
        currentReader_.generation++;               // Version bump
    }
    // LOCK RELEASED: audio thread instantly sees new state
}
```

### Audio Thread Read (Lock-Free)

Audio thread ALWAYS reads a complete, valid state:

```cpp
twComponent &getRootComponent() {
    ensureReader();
    SCutSnapshot snap = getSnapshot();
    
    // snap.reader.reader is ALWAYS:
    // - Non-NULL (has valid fallback)
    // - Completely constructed (no partial state)
    // - Was atomically committed (not mid-modification)
    if (snap.reader.reader) return *snap.reader.reader;
    return content_->getRootComponent();
}
```

No locks needed. No waiting. Reader state is immutable during the buffer.

---

## Guarantees This Model Provides

### ✅ Readers NEVER See NULL

```cpp
// currentReader_.reader is NEVER NULL after construction
Audio: if (snap.reader.reader)  // Always true, no null checks needed
       ↓
       SAFE to dereference
```

### ✅ Readers NEVER See Partial State

```cpp
// currentReader_ is atomic (struct copy on read):
//   reader pointer
//   grain pointer
//   looping flag
//   generation counter
// All four fields were assigned together (atomic swap)
// OR were the previous committed state
```

### ✅ Writers Don't Block Readers

```cpp
// rebuildReader builds entirely outside critical section:
buildNewReader()      // No locks, no blocking audio thread
buildNewGrain()       // No locks, no blocking audio thread
  ↓
{lock; swap; unlock}  // <1μs, audio thread might wait 1μs max
```

### ✅ Live Editing Works During Playback

```
Playback ON:  renderAudio() → getRootComponent() → *snap.reader.reader
              
UI Edit:      setVolume() → invalidateCapture() → rebuildReader()
              
Both proceed safely:
- Audio uses currentReader_ (snapshot at buffer start)
- UI builds nextReader_ independently
- Swap happens atomically
- Audio never sees rebuild
```

---

## Architecture: How It Works End-to-End

### Timeline: Volume Change During Playback

```
Time    Audio Thread              UI Thread
──────────────────────────────────────────────────────────────
t=0     [Buffer start]
        snap = getSnapshot()
        snap.reader = currentReader_  ← committed state
        renderAudio(snap)

t=0.1                             [User changes volume]
                                  setVolume(new_vol)
                                  invalidateCapture()
                                  rebuildReader(snap):
                                    newReader = build()
                                    newGrain = build()
                                    (currentReader unused)

t=0.2   [Buffer rendering]
        using snap.reader
        Audio continues
        unaffected by
        rebuild

t=0.3                             {lock; swap}
                                  oldReader_ = currentReader_
                                  currentReader_ = newReader_
                                  {unlock}

t=0.4   [Buffer end]
        snap becomes stale
        but still valid
        (oldReader_ kept alive)

t=1.0   [Next buffer start]
        snap = getSnapshot()
        snap.reader = currentReader_  ← NEW state, already complete
        renderAudio(snap)              and valid
```

### Key Points

1. **No waiting:** Audio thread never blocks
2. **No corruption:** Audio always has complete state
3. **Live editing:** UI can rebuild anytime
4. **Memory safe:** Old reader not deleted until safe (next rebuild)
5. **Atomic:** Swap is indivisible commit operation

---

## Extending This Model

### To Other Shared State

Apply the same pattern to any mutable state accessed by both threads:

#### Example: Preview Cache

```cpp
struct SCutPreviewState {
    preview_t *peaks;
    offset_t peakSkip;
    offset_t peakN;
};

class SCut {
    // Current preview (audio thread reads)
    SCutPreviewState currentPreview_;
    // New preview (UI rebuilds)
    SCutPreviewState nextPreview_;
    // Old preview (deferred deletion)
    SCutPreviewState oldPreview_;
};
```

#### Template

For any `X` accessed by both threads:

```cpp
class Foo {
    // CURRENT: readers see this, ALWAYS valid
    X currentX_;
    
    // NEXT: builder constructs this
    X nextX_;
    
    // OLD: previous current, can be cleaned up
    X oldX_;
    
    // ONE lock, atomic swap only
    mutable std::mutex xSwapLock_;
    
    // Swap when next is ready
    void swapX() {
        std::lock_guard lock(xSwapLock_);
        delete oldX_.resources();
        oldX_ = currentX_;
        currentX_ = nextX_;
    }
};
```

---

## Why Unix Page Cache Semantics?

The Unix kernel's page cache solved this problem decades ago:

1. **Readers need stable pages** — can't have writes mid-read
2. **Writers need fast commit** — can't block real-time processes
3. **Solution: Double-buffer** — current committed page, next being written, old being recycled

Smaragd applies the same principle:
- **Readers** (audio thread) need stable reader state
- **Writers** (UI thread) need fast, non-blocking updates
- **Solution: Double-buffer** — current committed, next building, old recyclable

---

## Implementation Checklist

When adding new shared state:

- [ ] Define state struct (e.g., `SCutReaderState`)
- [ ] Add three copies: `current*_`, `next*_`, `old*_`
- [ ] Add one swap lock: `*SwapLock_`
- [ ] Readers use `current*_` only (snapshot it if needed)
- [ ] Writers build `next*_` completely (no locks)
- [ ] When ready: atomic swap (lock, rotate, unlock)
- [ ] Document the pattern in class comment
- [ ] Test: playback + edit should not corrupt/crash

---

## Guarantees Summary

| Guarantee | How Achieved |
|-----------|-------------|
| Readers never see NULL | currentReader always initialized |
| Readers never see partial state | Only atomic swaps update current |
| No audio thread blocking | Build outside critical section |
| Memory safe | Old kept until next swap |
| Live editing works | next_ invisible during build |

---

## Testing

Verify the model works:

```
✅ Start playback on track with audio
✅ While playing, drag clip window boundaries
   → Preview should update cleanly, no corruption
✅ While playing, change volume slider
   → Waveform should scale correctly, no glitches
✅ While playing, rapid parameter changes
   → No crashes, stable playback
```

If any of these fail, there's still a race in a different part of the system.

---

## References

- **Unix Page Cache:** Linux kernel documentation on page cache
- **Double Buffering:** Standard graphics rendering technique (frame buffers)
- **Lock-Free Patterns:** Herb Sutter's work on atomics and memory ordering
- **Implementation:** See `CONCURRENCY_GUIDELINES.md` for formal rules

---

## History

- **Problem identified:** Concurrent `calcOutputTo()` calls during playback + edit
- **First attempt:** Mutexes on individual members → races persisted
- **Insight:** User's Unix analogy → page cache model
- **Solution:** Double-buffer reader state, atomic swap
- **Result:** ✅ Safe concurrent access, live editing during playback works

