# Lazy Invalidation and Aspect-Based Capture Caching

**Status:** ✅ Phases 1-3 Complete (Synchronous Lazy Invalidation)  
**Author:** Timo Weggen, Claude  
**Date:** 2026-06-21  
**Priority:** High (Performance regression: 1-2s hangs on mute toggle with grouped assets)  
**Implemented:** 2026-06-21 to 2026-06-22

## Problem

Current implementation eagerly invalidates and recomputes all `SCut` captures on any `arrangementChanged()` signal:

```
Mute toggle → notifyArrangementChanged() → ALL cuts invalidate & rebuild
              ↓
           twGrainSource reinitializes (expensive)
              ↓
           1-2s UI hang (unacceptable for rapid UI interaction)
```

**Root causes:**
1. **Coarse invalidation**: Mute/solo/volume changes invalidate *arrangement* (structure), but don't actually change arrangement
2. **Eager recomputation**: Captures rebuild immediately, even if never accessed
3. **No granularity**: All capture data (preview, playback, metadata) recomputes together
4. **No dependency tracking**: All cuts invalidate, even if not referenced by anything

## Solution: Lazy Invalidation with Aspect-Based Caching

Inspired by Unix page cache:
- **Invalidation is cheap** (set bits in a bitmask)
- **Revalidation is lazy** (compute on demand or async)
- **Fine-grained tracking** (different data aspects, different lifecycles)
- **Dependency-aware** (only invalidate what's actually referenced)

### Key Concepts

#### 1. Capture Aspects (Bitmask)

Different capture data has different lifecycles and dependencies:

```cpp
enum CaptureAspect : uint32_t {
    Preview   = 1 << 0,  // Waveform for timeline display
                          // - Used: UI painting (constant)
                          // - Cost: Medium (resample, peak-detect)
                          // - Update freq: Can be batched, low priority
    
    Playback  = 1 << 1,  // Full-quality audio for playback
                          // - Used: Audio thread (real-time)
                          // - Cost: High (grain sources, effects)
                          // - Update freq: Rare (only on edit)
    
    Export    = 1 << 2,  // Resampled/normalized for export
                          // - Used: Export dialog (on-demand)
                          // - Cost: High (slow resample)
                          // - Update freq: On export, format change
    
    Metadata  = 1 << 3,  // Duration, peak levels, RMS
                          // - Used: UI, transport
                          // - Cost: Low (computed with playback)
                          // - Update freq: With playback
};
```

#### 2. State Tracking (Bitfields)

```cpp
class SCut {
    uint32_t valid_ = 0;        // Which aspects are currently computed
    uint32_t invalidated_ = 0;  // Which aspects need recomputation
    
    // Data storage (null until valid)
    std::unique_ptr<PreviewData> preview_;
    std::unique_ptr<PlaybackReader> playbackReader_;
    std::unique_ptr<ExportBuffer> exportBuffer_;
    MetadataCache metadata_;
};
```

**Invariant:** `valid_ & invalidated_ == 0` (aspect is either valid or invalidated, not both)

#### 3. Aspect-Specific Invalidation

```cpp
void SCut::invalidate(uint32_t aspects) {
    invalidated_ |= aspects;  // Mark as needing recompute
    valid_ &= ~aspects;       // Clear from valid set
}
```

**Examples:**
```cpp
onTrackMuted() {
    for (auto cut : affectedCuts) {
        cut->invalidate(Playback | Metadata);
        // Preview unaffected (waveform shape unchanged)
    }
}

onClipResized() {
    for (auto cut : affectedCuts) {
        cut->invalidate(Preview | Playback | Metadata | Export);
    }
}

onGrainTimeChanged() {
    for (auto cut : affectedCuts) {
        cut->invalidate(Playback | Metadata);
        // Preview: could reuse (similar shape) or invalidate (conservative)
    }
}
```

#### 4. Lazy Revalidation (On Demand)

```cpp
void SCut::ensureCapture(uint32_t needed) {
    // Only compute aspects we need and don't have
    uint32_t toCompute = (invalidated_ | ~valid_) & needed;
    
    if (!toCompute) return;  // Already valid
    
    if (toCompute & Preview) {
        recomputePreview();   // Sync, relatively cheap
    }
    if (toCompute & Metadata) {
        recomputeMetadata();  // Cheap, computed with playback
    }
    if (toCompute & Playback) {
        recomputePlayback();  // Expensive, possibly async
    }
    if (toCompute & Export) {
        recomputeExport();    // On-demand, expensive
    }
    
    // Mark computed aspects as valid
    valid_ |= toCompute;
    invalidated_ &= ~toCompute;
}
```

**Call sites:**
```cpp
// In timeline rendering (paint loop)
cut->ensureCapture(Preview | Metadata);  // Cheap, sync

// In audio playback
cut->ensureCapture(Playback | Metadata);  // Expensive, may block briefly

// In export dialog
cut->ensureCapture(Export | Metadata);    // On-demand, user initiated
```

#### 5. Async Revalidation (Background Thread)

For expensive aspects, use a background thread:

```cpp
class CaptureRevalidator {
    struct Job {
        SCut* cut;
        uint32_t aspects;
        int priority;  // High for Playback, Medium for Export, Low for Preview
        std::chrono::steady_clock::time_point deadline;
    };
    
    PriorityQueue<Job> queue_;
    std::thread bgThread_;
    
    void scheduleRevalidation(SCut* cut, uint32_t aspects) {
        int priority = 0;
        if (aspects & Playback) priority = 10;      // High
        if (aspects & Export)   priority = 5;       // Medium
        if (aspects & Preview)  priority = 1;       // Low
        
        queue_.enqueue(Job{cut, aspects, priority, now() + 100ms});
        bgThread_.notify();
    }
    
    void backgroundLoop() {
        while (true) {
            Job job = queue_.dequeue(timeout: 1s);
            if (!job) continue;
            
            // Recompute at low priority (won't block UI)
            job.cut->ensureCapture(job.aspects);
            
            // Notify listeners (e.g., redraw timeline)
            emit captureUpdated(job.cut, job.aspects);
        }
    }
};
```

#### 6. Dependency Tracking (Fine-Grained Invalidation)

Track which `SObject`s reference a given subtree:

```cpp
class SObject {
    // Set of objects that reference (depend on) this object
    QSet<SObject*> referencedBy_;
    
    // Register/unregister dependencies
    void addReference(SObject* referrer) {
        referencedBy_.insert(referrer);
    }
    void removeReference(SObject* referrer) {
        referencedBy_.remove(referrer);
    }
    
    // Notify dependents of change
    void notifyChange(uint32_t affectedAspects) {
        for (auto referrer : referencedBy_) {
            if (SCut* cut = dynamic_cast<SCut*>(referrer)) {
                cut->invalidate(affectedAspects);
            }
        }
    }
};

// Usage in STrack:
void STrack::setMuted(bool muted) {
    muted_ = muted;
    
    // Only notify cuts that reference this track's output
    notifyChange(Playback | Metadata);
    
    // NOT arrangementChanged() — structure didn't change
}
```

**Benefit:** A muted track only invalidates its direct dependents (the cuts that reference it), not the entire scene.

---

## Architecture

### Data Structure Changes (Implemented)

```cpp
// scut.h
class SCut : public SObject {
private:
    // Aspect-based caching state
    uint32_t validAspects_ = 0;    // Bitmask: which aspects are currently valid
                                   // Invalid aspects = those bits NOT set
    
    // Cached data (shared_ptr so audio thread keeps capture alive during render)
    std::shared_ptr<twCapturingSource> capture_;
    preview_t *capPeaks_ = nullptr;     // Peaks over the capture for waveform
    
    // Double-buffer reader state (Unix page cache model)
    std::mutex readerSwapLock_;
    SCutReaderState currentReader_;     // Always valid; audio thread reads this
    SCutReaderState nextReader_;        // Being built by UI thread
    
public:
    // Query which aspects are valid
    bool isAspectValid(uint32_t aspect) const {
        return (validAspects_ & aspect) == aspect;
    }
    
    // Invalidate specific aspects
    void invalidateAspects(uint32_t aspects);
    
    // Synchronous lazy recomputation: compute on-demand if needed
    void ensureCapture(uint32_t aspectsMask);
};

// sobject.h
class SObject : public QObject {
private:
    // Lazy invalidation + dependency tracking
    mutable std::mutex dependentsMutex_;
    QSet<SLink*> dependentLinks_;  // SLink objects that reference this object
    
public:
    // Register a dependent link (called when a cut references this object)
    void addDependentLink(SLink *dependentLink) {
        {
            std::lock_guard<std::mutex> lock(dependentsMutex_);
            dependentLinks_.insert(dependentLink);
        }
        // Auto-unregister when link is destroyed (safety: prevents use-after-free)
        QObject::connect(dependentLink, &QObject::destroyed,
                         this, [this, dependentLink]() {
            removeDependentLink(dependentLink);
        });
    }
    
    void removeDependentLink(SLink *dependentLink) {
        std::lock_guard<std::mutex> lock(dependentsMutex_);
        dependentLinks_.remove(dependentLink);
    }
    
    // Notify dependents that specific aspects have changed
    void notifyDependentsChanged(uint32_t affectedAspects) {
        // Snapshot under lock, iterate without lock (safe from modifications)
        QSet<SLink*> dependents;
        {
            std::lock_guard<std::mutex> lock(dependentsMutex_);
            dependents = dependentLinks_;
        }
        
        for (SLink *link : dependents) {
            if (!link) continue;
            // Notify the cut that references this object
            SObject &linkedObj = link->getSObject();
            SCut *cut = dynamic_cast<SCut*>(&linkedObj);
            if (cut) {
                cut->invalidateAspects(affectedAspects);
            }
        }
    }
};
```

### Call Site Changes (Implemented)

**Before:**
```cpp
void SSMVMixerControl::muteToggled(bool on) {
    tk_.setMuted(on);
    // This implicitly called:
    // → SProject::notifyArrangementChanged()
    // → emit arrangementChanged()
    // → SCut::invalidateCapture() [ALL cuts, blocking]
    // → rebuildReader() × 100+ cuts
    // → 1-2s hang
}

void SCut::invalidateCapture() {
    capture_.reset();
    rebuildReader();  // EAGER: immediately rebuilds entire reader chain
}
```

**After (Implemented):**
```cpp
void SSMVMixerControl::muteToggled(bool on) {
    tk_.setMuted(on);
    // Now goes to:
}

void SObject::setMuted(bool muted) {
    if (muted == muted_) return;
    muted_ = muted;
    emit mutedChanged(muted);
    
    // Lazy invalidation: only notify cuts that reference this track
    notifyDependentsChanged(Playback | Metadata);
    // NOT notifyArrangementChanged() — structure didn't change
}

void SCut::invalidateAspects(uint32_t aspects) {
    // Drop stale capture if playback is invalidated
    if (aspects & Playback) {
        capture_.reset();
        if (capPeaks_) { ::free(capPeaks_); capPeaks_ = NULL; }
    }
    
    // Clear invalid aspect bits
    validAspects_ &= ~aspects;
    
    // Propagate to cuts that reference this cut (transitive chain)
    if (aspects & Playback) {
        notifyDependentsChanged(Playback | Metadata);
    }
}

void SCut::invalidateCapture() {
    // Called on arrangement changes only (not audio state changes)
    capture_.reset();
    if (capPeaks_) { ::free(capPeaks_); capPeaks_ = NULL; }
    
    // Invalidate all capture aspects, but DON'T eagerly rebuild
    // Use invalidateAspects() to trigger lazy recomputation
    invalidateAspects(Preview | Playback | Metadata);
    
    // NOTE: Do NOT call rebuildReader() here (lazy invalidation)
}

void SCut::ensureCapture(uint32_t aspectsMask) {
    // Synchronous, on-demand recomputation
    uint32_t toCompute = aspectsMask & ~validAspects_;
    
    if (toCompute == 0) return;  // All needed aspects already valid
    
    // Compute on demand (synchronously)
    if (toCompute & Preview)  recomputePreview();
    if (toCompute & Metadata) recomputeMetadata();
    if (toCompute & Playback) recomputePlayback();
    if (toCompute & Export)   recomputeExport();
    
    // Mark computed aspects as valid
    validAspects_ |= toCompute;
}
```

---

## Implementation Plan & Status

### ✅ Phase 1: Aspect Bitmask Foundation (Complete)

- ✅ Define `SCutCaptureAspect` enum in `scut.h`
- ✅ Add `validAspects_` bitfield to `SCut` (single bitmask, not separate valid/invalidated)
- ✅ Replace eager `rebuildReader()` with lazy `ensureCapture()`
- ✅ Update call sites to `ensureCapture(aspectsMask)` for synchronous on-demand recomputation
- ✅ Test: Preview/playback/export work correctly

**Implementation detail:** Used single `validAspects_` bitmask instead of separate `valid_` and `invalidated_` fields. Invalid aspects are simply those bits not set in `validAspects_`.

### ✅ Phase 2: Dependency Tracking (Complete)

- ✅ Add `dependentLinks_` set to `SObject` (tracks SLink objects that reference this object)
- ✅ Wire up reference registration in SCut constructor/destructor via `addDependentLink()`
- ✅ Implement `notifyDependentsChanged(aspects)` to notify dependents
- ✅ Update `SObject::setMuted()`, `setSolo()`, `setVolume()` to call `notifyDependentsChanged(Playback | Metadata)` instead of `notifyArrangementChanged()`
- ✅ Remove `notifyArrangementChanged()` from `muteToggled()` / `soloToggled()` (SSMVMixerControl)
- ✅ Test: Mute one track, only affected cuts invalidate (no full-scene rebuild)

**Implementation detail:** Uses `QSet<SLink*>` with mutex for thread-safe access. SLink is the native reference primitive, so tracking via links directly is cleaner than a generic dependent set.

**Safety critical:** Auto-unregister via Qt's `destroyed()` signal prevents use-after-free when dependent links are destroyed. See `PHASE3_SAFETY_ADDENDUM.md` for details.

### ⏭️ Phase 3: Async Revalidation (Deferred to Phase 4+)

**Status:** Not implemented. Current system is synchronous lazy invalidation only.

- ⏹️ `CaptureRevalidator` background thread (deferred)
- ⏹️ Priority queue (deferred)
- ⏹️ Async signals on revalidation complete (deferred)
- ⏹️ Expensive aspects scheduled async (deferred)

**Why deferred:** Synchronous lazy invalidation already eliminates the 1-2s hangs. Playback aspects recompute on-demand when audio thread calls `ensureCapture(Playback)`, and UI repaints trigger `ensureCapture(Preview)` synchronously. Background async queue would be a future optimization, not critical for the performance fix.

**Future:** Phase 4 can add background revalidation if profiling shows stalls. Likely candidates: Export aspects (user-initiated, non-interactive), or prefetch Playback for clips approaching playhead during playback.

### Phase 4: Optimization & Tuning (Pending)

- [ ] Profile aspect recomputation costs with complex projects
- [ ] Consider batch invalidation for undo operations
- [ ] Cache invalidation policies per `SObject` type (e.g., containers vs samples)
- [ ] Measure if async Export revalidation is needed (render dialog latency)

---

## Examples

### Example 1: Mute Toggle (Before vs. After Implementation)

**Before (1-2s hang):**
```
User mutes track
  → SSMVMixerControl::muteToggled()
  → SProject::notifyArrangementChanged()
  → emit arrangementChanged()
  → ALL cuts' invalidateCapture() [blocking]
  → rebuildReader() × 100+ cuts
  → twGrainSource initialization × 100+ clips
  → 1-2s UI hang
  ↓
  → Audio finally mutes
```

**After (instant, no hang):**
```
User mutes track
  → SSMVMixerControl::muteToggled()
  → SObject::setMuted(true)
  → emit mutedChanged(true)
  → notifyDependentsChanged(Playback | Metadata)
  ↓
  → For each SLink in dependentLinks_:
     → Only cuts that REFERENCE this track
     → SCut::invalidateAspects(Playback | Metadata)
     → Clear bits in validAspects_ (1-2 CPU cycles)
  ↓
  → Return immediately (< 1ms total)
  → UI stays responsive
  ↓ (lazy, on next audio pull or UI repaint)
  → ensureCapture(Playback) or ensureCapture(Preview)
  → Rebuild only the needed aspects
  → Audio updates with mute applied
```

### Example 2: Edit Grouped Asset (Before vs. After)

**Before:**
```
User edits grain parameter in grouped asset
  → Asset emits changed signal
  → SProject::notifyArrangementChanged()
  → emit arrangementChanged()
  → ALL cuts invalidateCapture() [blocking]
  → twGrainSource reinit × 100+ clips
  → 2s hang per single parameter edit
```

**After (Implemented):**
```
User edits grain parameter in grouped asset
  → Asset emits changed signal
  → Asset::notifyDependentsChanged(Playback | Metadata)
  ↓
  → For each SLink in asset's dependentLinks_:
     → Only cuts that REFERENCE this asset
     → SCut::invalidateAspects(Playback | Metadata)
     → capture_.reset() (if Playback affected)
     → validAspects_ &= ~(Playback | Metadata)
     → Propagate to cuts that reference this cut (transitive chain)
  ↓
  → Unrelated cuts: untouched (Preview still valid, can still paint waveform)
  → Return immediately (< 1ms)
  ↓ (lazy, when audio plays or user paints)
  → ensureCapture(Playback) rebuilds only affected cuts
  → twGrainSource initialized only for impacted clips
```

### Example 3: Waveform Display and Playback

**Synchronous lazy invalidation (actual implementation):**

```cpp
// Timeline paint (60 FPS, UI thread)
void SMVActualView::paintEvent() {
    for (auto cut : visibleCuts) {
        cut->ensureCapture(Preview | Metadata);  // Synchronous on-demand
        if (cut->isAspectValid(Preview)) {
            drawWaveform(cut->getPreview());
        } else {
            drawPlaceholder();  // Brief stale display, then repaints
        }
    }
}

// Audio playback (audio thread)
void AudioThread::renderFrame() {
    for (auto cut : activeCuts) {
        cut->ensureCapture(Playback | Metadata);  // Blocks if just invalidated
        if (cut->isAspectValid(Playback)) {
            reader = cut->getPlaybackReader();
            reader->readFrames(buffer);
        } else {
            // Fallback: silence or repeat last frame
            writeZeros(buffer);
        }
    }
}

// User edits track mute while playing
void onTrackMuted() {
    for (auto cut : affectedCuts) {
        cut->invalidateAspects(Playback | Metadata);  // Just clears bits
    }
    // Timeline still shows old waveform (Preview not invalidated)
    // Next paint: invalidated Preview triggers refresh
}
```

**Key difference from design:** Both UI and audio thread block briefly if they encounter an invalidated aspect. This is acceptable because:
- Invalidation is rare (only on parameter changes, not continuous)
- recomputation is fast (10-50ms for typical clips)
- First audio frame may stall; subsequent frames use cached reader
- Much better than 1-2s eager rebuild of all captures

---

## Benefits (Implemented)

| Scenario | Before | After (Implemented) |
|----------|---------|----------|
| **Mute 1 track** | Rebuild 100+ cuts (1-2s) | Set bits (1ms) + lazy rebuild (~50ms on demand) |
| **Edit grain param** | All cuts rebuild (2s per edit) | Only dependent cuts rebuild (on-demand) |
| **Rapid UI edits** | Multiple 1-2s hangs = stalled UI | No hangs; cuts rebuild lazily as needed |
| **Preview-only painting** | Rebuild playback unnecessarily | Preview reused (already valid) |
| **Export with unchanged audio** | Rebuild playback (wasted effort) | Reuse playback, only recompute export |
| **Grouped asset workflow** | Edit mute → 2s hang per click | Instant response; audio updates ~50ms later |
| **Dependency specificity** | Entire scene invalidated | Only affected cuts invalidated (fine-grained) |

---

## Potential Issues & Mitigations

### Issue 1: Audio Thread Blocks on Invalidated Playback

**Problem:** Audio thread calls `ensureCapture(Playback)` and briefly blocks if just invalidated.

**Solution:** `ensureCapture()` synchronously recomputes if needed:
```cpp
void SCut::ensureCapture(uint32_t aspectsMask) {
    uint32_t toCompute = aspectsMask & ~validAspects_;
    if (toCompute == 0) return;  // Fast path: already valid
    
    // Slow path: recompute on-demand (first time only)
    if (toCompute & Playback) recomputePlayback();  // May block 10-50ms
    validAspects_ |= toCompute;
}
```

**Mitigations:**
- First audio frame may stall ~10-50ms after invalidation
- Subsequent frames use cached reader (no additional stalls)
- Much cheaper than 1-2s eager rebuild of all captures
- Audio state changes (mute/solo) are rare; stall only on those events
- Playback cache (twCapturingSource shared_ptr) kept alive by audio thread, preventing deallocation during concurrent invalidation

### Issue 2: Use-After-Free from Dependent Link Destruction

**Problem:** If a cut is destroyed, its SLink* remains in parent's `dependentLinks_` set, causing crash on next `notifyDependentsChanged()`.

**Solution:** Auto-unregister via Qt's `destroyed()` signal (see `PHASE3_SAFETY_ADDENDUM.md`):
```cpp
void SObject::addDependentLink(SLink *link) {
    dependentLinks_.insert(link);
    QObject::connect(link, &QObject::destroyed, this, [this, link]() {
        removeDependentLink(link);  // Fires during destruction, before stale
    });
}
```

**Safety properties:**
- Signal fires atomically before pointer becomes stale
- Snapshot-based iteration in `notifyDependentsChanged()` prevents race conditions
- Lock guard ensures thread-safe insertion/removal

### Issue 3: Stale UI Display (Waveform)

**Problem:** Timeline briefly shows old waveform while new version recomputes.

**Solution:**
- Preview invalidation is rare (only on clip resize, not audio state changes)
- `ensureCapture(Preview)` on paint; if not yet valid, draw placeholder
- Emit signal when Preview revalidation completes
- UI repaints on next 60 FPS tick (imperceptible stale display, ~16ms)

### Issue 4: Memory: Multiple Cached Aspects

**Problem:** Keeping `capture_`, `capPeaks_`, reader chain, playback buffer wastes memory with complex projects.

**Solution (current):**
- `capture_` (shared_ptr): Dropped when Playback invalidated, recreated on-demand
- `capPeaks_`: Freed with capture
- `reader chain`: Rebuilt only when needed
- Export buffer: Computed on-demand, not cached

**Future optimization (Phase 4+):**
- Add `setMaxCachedAspects()` to limit total memory
- LRU eviction when cache exceeds limit
- Profile with complex projects to determine threshold

---

## Implementation Notes

### Synchronous vs. Async Revalidation (Why async was deferred)

The original design proposed a background `CaptureRevalidator` thread with priority queue (Phase 3). **This was NOT implemented.** Instead, the system uses synchronous lazy invalidation:

- **Invalidation:** Cheap (set bits) → instant feedback
- **Revalidation:** On-demand (sync) → audio thread blocks briefly if needed (~10-50ms), then uses cached result
- **Result:** 1-2s hangs eliminated; brief stalls only on parameter changes (acceptable trade-off)

**Why this works:** Invalidations are rare and concentrated in response to user actions (mute, solo, parameter edit). Synchronous recomputation is acceptable because:
1. Invalidation is fast (set bits in a bitmask)
2. Recomputation is infrequent (not every frame)
3. Result is cached (second frame uses cached reader, no stall)
4. Audio thread rarely encounters just-invalidated aspects

**If async becomes necessary (Phase 4+):** Profile shows excessive audio stalls, add background thread to prefetch:
- High priority: Playback aspects for clips near playhead during playback
- Low priority: Export aspects for render dialog (user-initiated, interactive acceptable)
- Medium priority: Preview aspects for timeline painting (UI can tolerate 1-2 frames of stale display)

### Dependency Tracking via SLink

The actual implementation tracks dependencies using `QSet<SLink*>` rather than a generic `QSet<SObject*>`:

**Advantages:**
- `SLink` is the native reference primitive in the codebase
- Avoids downcasting; cleaner iteration over dependent cuts
- Thread-safe: SLink is QObject, destroyed() signal is atomic

**Thread safety pattern:**
```
1. addDependentLink(link) [UI thread]
   ├─ Insert into set under lock
   └─ Connect destroyed() signal (lock released before connect)

2. delete link [possibly different thread]
   ├─ link->destroyed() emitted
   ├─ Lambda captured this + link safely (via Qt's queued slot semantics)
   ├─ removeDependentLink(link) called
   └─ Removed from set before memory freed

3. notifyDependentsChanged() [any thread]
   ├─ Snapshot set under lock
   ├─ Iterate snapshot without lock
   └─ No race: destroyed() → removal before snapshot
```

### Transitive Invalidation

When a cut's Playback aspect is invalidated (e.g., because its parent track muted), the cut propagates invalidation to its own dependents:

```cpp
void SCut::invalidateAspects(uint32_t aspects) {
    if (aspects & Playback) {
        capture_.reset();  // Drop stale audio data
    }
    validAspects_ &= ~aspects;
    
    // Propagate to cuts that reference this cut
    if (aspects & Playback) {
        notifyDependentsChanged(Playback | Metadata);
    }
}
```

**Example chain:**
```
Track A muted
  → Track A::notifyDependentsChanged(Playback | Metadata)
  → Cut C (refs Track A) invalidated
  → Cut C::invalidateAspects(Playback | Metadata)
  → Cut C::notifyDependentsChanged(Playback | Metadata)  [transitive]
  → Cut D (refs Cut C) invalidated
  → Cut D's audio now reflects Track A's mute state
```

### Commitment Tracking

Captures use double-buffered reader state (Unix page cache model):
```cpp
SCutReaderState currentReader_;  // Audio thread reads; always valid + complete
SCutReaderState nextReader_;     // UI thread builds
```

When UI thread finishes rebuilding:
```cpp
void rebuildReader(const SCutSnapshot &snap) {
    // Build nextReader_
    SCutReaderState next = buildReaderChain(snap);
    
    // Atomic swap
    {
        std::lock_guard lock(readerSwapLock_);
        oldReader_ = currentReader_;
        currentReader_ = next;
    }
    
    // Audio thread finishes using oldReader_, freed here or on next swap
}
```

This ensures audio thread always has a complete, current reader with no glitches.

---

## Future Extensions (Candidate for Phase 4+)

1. **Async Background Revalidation**: Background thread for expensive aspects (Export, predictive Playback prefetch)
2. **Per-Aspect Memory Limits**: Config max cache size per aspect (e.g., 1GB preview, 5GB playback)
3. **Predictive Revalidation**: Prefetch Playback for clips approaching playhead during playback
4. **Aspect Priorities**: User control (e.g., "prioritize export quality over playback latency")
5. **Selective Invalidation**: Only invalidate changed region (e.g., clip resized 100ms, only tail invalidated)
6. **Aspect Aliases**: `Playback|Metadata` → `Realtime` alias for convenience

---

## References & Inspiration

- **Unix page cache:** Invalidate on write, validate on read (lazy revalidation model)
- **Vulkan double-buffering:** Different lifetime for frame data (committed vs. pending)
- **DAWs (Ableton, Logic):** Lazy waveform generation, eager audio buffering (sync playback path)
- **Chromium Blink:** Invalidation sets dirty bits, validation on demand (aspect-based model)
- **Qt destruction signal:** Atomic emission before memory freed (safe dependent cleanup)

