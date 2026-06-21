# Lazy Invalidation and Aspect-Based Capture Caching

**Status:** Design  
**Author:** Timo Weggen, Claude  
**Date:** 2026-06-21  
**Priority:** High (Performance regression: 1-2s hangs on mute toggle with grouped assets)

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

### Data Structure Changes

```cpp
// scut.h
class SCut {
private:
    // Current state
    uint32_t valid_ = 0;           // Bitmask: computed aspects
    uint32_t invalidated_ = 0;     // Bitmask: needs recompute
    
    // Cached data (null until computed)
    std::unique_ptr<PreviewData> preview_;
    std::unique_ptr<PlaybackReader> playbackReader_;
    std::unique_ptr<ExportBuffer> exportBuffer_;
    MetadataCache metadata_;
    
    // For background revalidation
    CaptureRevalidationJob* bgJob_ = nullptr;
    
public:
    // Query state
    bool isValid(uint32_t aspect) const {
        return (valid_ & aspect) == aspect;
    }
    bool isInvalidated(uint32_t aspect) const {
        return (invalidated_ & aspect) == aspect;
    }
    
    // Invalidate specific aspects
    void invalidate(uint32_t aspects);
    
    // Ensure aspects are computed (lazy)
    void ensureCapture(uint32_t needed);
    
    // Accessors (trigger ensureCapture)
    const PreviewData& getPreview() {
        ensureCapture(Preview);
        return *preview_;
    }
    
    PlaybackReader& getPlaybackReader() {
        ensureCapture(Playback | Metadata);
        return *playbackReader_;
    }
};

// sproject.h
class SProject {
    // Global revalidation queue
    std::unique_ptr<CaptureRevalidator> revalidator_;
    
public:
    void scheduleAsyncRevalidation(SCut* cut, uint32_t aspects) {
        revalidator_->scheduleRevalidation(cut, aspects);
    }
};

// sobject.h
class SObject {
    QSet<SObject*> referencedBy_;  // Objects that depend on me
    
    void addReference(SObject* referrer) {
        referencedBy_.insert(referrer);
    }
    void removeReference(SObject* referrer) {
        referencedBy_.remove(referrer);
    }
    
    // Notify specific dependents of change
    void notifyChange(uint32_t affectedAspects) {
        for (auto referrer : referencedBy_) {
            referrer->onDependencyChanged(this, affectedAspects);
        }
    }
    
protected:
    virtual void onDependencyChanged(SObject* source, uint32_t aspects) {
        // Subclasses override to handle invalidation
    }
};
```

### Call Site Changes

**Before:**
```cpp
void SProject::notifyArrangementChanged() {
    emit arrangementChanged();  // Invalidates ALL captures
}

// Leads to:
void SCut::invalidateCapture() {
    rebuildReader();  // Eager, blocks UI
}
```

**After:**
```cpp
void SProject::notifyArrangementChanged() {
    // Only emit if structure actually changed
    emit arrangementChanged();
}

void STrack::setMuted(bool muted) {
    muted_ = muted;
    
    // Notify cuts this track affects
    notifyChange(Playback | Metadata);
    // NOT arrangementChanged() — structure didn't change
}

void SCut::invalidate(uint32_t aspects) {
    invalidated_ |= aspects;      // Just set bits (cheap)
    valid_ &= ~aspects;
    
    // Schedule async revalidation for expensive aspects
    if (aspects & (Playback | Export)) {
        project->scheduleAsyncRevalidation(this, aspects & (Playback | Export));
    }
}

void SCut::ensureCapture(uint32_t needed) {
    uint32_t toCompute = (invalidated_ | ~valid_) & needed;
    if (!toCompute) return;
    
    // Compute on demand
    if (toCompute & Preview)  recomputePreview();
    if (toCompute & Metadata) recomputeMetadata();
    if (toCompute & Playback) recomputePlayback();
    if (toCompute & Export)   recomputeExport();
    
    valid_ |= toCompute;
    invalidated_ &= ~toCompute;
}
```

---

## Implementation Plan

### Phase 1: Aspect Bitmask Foundation

- [ ] Define `CaptureAspect` enum in `scut.h`
- [ ] Add `valid_` and `invalidated_` bitfields to `SCut`
- [ ] Replace eager `rebuildReader()` with lazy `ensureCapture()`
- [ ] Update all call sites to `ensureCapture(needed)` instead of direct access
- [ ] Test: Verify preview/playback/export still work

### Phase 2: Dependency Tracking

- [ ] Add `referencedBy_` set to `SObject`
- [ ] Wire up reference registration (when asset is placed, clip is added, etc.)
- [ ] Implement `notifyChange(aspects)` to notify dependents
- [ ] Update `STrack`, `SStdMixer` to call `notifyChange()` instead of `notifyArrangementChanged()`
- [ ] Test: Mute one track, verify only affected cuts invalidate

### Phase 3: Async Revalidation

- [ ] Implement `CaptureRevalidator` background thread
- [ ] Priority queue (Playback > Export > Preview)
- [ ] Emit signals when revalidation completes (for UI redraw)
- [ ] Call sites schedule expensive aspects (`Playback`, `Export`) async
- [ ] Test: Mute toggle is instant, playback audio updates within 100ms

### Phase 4: Optimization & Tuning

- [ ] Profile aspect recomputation costs
- [ ] Batch related invalidations (e.g., undo clears multiple aspects)
- [ ] Cache invalidation policies per `SObject` type
- [ ] Test with complex projects (many clips, grouped assets, rapid edits)

---

## Examples

### Example 1: Mute Toggle (Current vs. Proposed)

**Current:**
```
User mutes track
  → SSMVMixerControl::muteToggled()
  → SProject::notifyArrangementChanged()
  → emit arrangementChanged()
  → SCut::invalidateCapture() [ALL cuts, blocking]
  → twGrainSource::twGrainSource() × 100 clips
  → 1-2s hang
  → Audio mutes
```

**Proposed:**
```
User mutes track
  → SSMVMixerControl::muteToggled()
  → STrack::setMuted()
  → notifyChange(Playback | Metadata)
  → SCut::invalidate(Playback | Metadata)
  → valid_ &= ~(Playback | Metadata)
  → scheduleAsyncRevalidation() [background thread]
  → Return immediately (no hang)
  → UI stays responsive
  ↓ (async, 10-100ms later)
  → CaptureRevalidator recomputes affected cuts
  → Audio updates (imperceptibly late)
```

### Example 2: Edit Grouped Asset

**Current:**
```
Edit grain parameter in grouped asset
  → notifyArrangementChanged()
  → ALL cuts invalidate
  → 2s hang per edit
```

**Proposed:**
```
Edit grain parameter in grouped asset
  → SObject::notifyChange(Playback)
  → Only cuts that reference this asset invalidate
  → Unrelated cuts: untouched (Preview still valid)
  → Async revalidation: playback updates ~50ms later
```

### Example 3: Waveform Display During Playback

```cpp
// Timeline paint (60 FPS)
void SMVActualView::paintEvent() {
    for (auto cut : visibleCuts) {
        cut->ensureCapture(Preview | Metadata);  // Cheap, sync
        drawWaveform(cut->getPreview());
    }
}

// Audio playback
void AudioThread::renderFrame() {
    for (auto cut : activeCuts) {
        cut->ensureCapture(Playback | Metadata);  // May block briefly if just invalidated
        playbackReader->readFrames(buffer);
    }
}

// User edits while playing
void onEdit() {
    cut->invalidate(Playback);
    // Preview stays valid: waveform looks same, just audio will differ
}
```

---

## Benefits

| Scenario | Current | Proposed |
|----------|---------|----------|
| **Mute 1 track** | Rebuild 100+ cuts (2s) | Set 1 flag (1ms) + async (50ms) |
| **Edit grain param** | All cuts rebuild (2s) | Only dependents rebuild async |
| **Rapid UI edits** | Multiple 1-2s hangs | Single batched background pass |
| **Preview-only painting** | Rebuild playback unnecessarily | Preview reused (already valid) |
| **Export with unchanged audio** | Rebuild playback (wasted) | Reuse playback, only export |
| **Background work** | None (eager blocking) | Async revalidation at low priority |

---

## Potential Issues & Mitigations

### Issue 1: Audio Thread Sees Stale Playback Data

**Problem:** Audio thread calls `getPlaybackReader()` before async revalidation completes.

**Solution:** `ensureCapture()` blocks until aspect is valid:
```cpp
PlaybackReader& SCut::getPlaybackReader() {
    ensureCapture(Playback | Metadata);  // Blocks if needed
    return *playbackReader_;
}
```
If playback is still being revalidated in background, audio thread briefly blocks (~10-50ms max). Acceptable because:
- First audio frame waits for revalidation
- Subsequent frames use cached playback reader
- Much cheaper than rebuilding from scratch

### Issue 2: Memory: Multiple Copies of Data

**Problem:** Keeping `preview_`, `playbackReader_`, `exportBuffer_` wastes memory.

**Solution:** 
- `Preview`: Keep (small, ~100KB per clip)
- `PlaybackReader`: Keep (usually needed, ~1MB per clip)
- `Export`: Compute on-demand only (keep null until needed)
- Provide `setMaxCachedAspects()` to limit memory

### Issue 3: Stale Data Appears Briefly

**Problem:** Timeline shows old waveform while new version computes in background.

**Solution:**
- Preview invalidation is rare (only on clip resize)
- When invalidated, immediately show placeholder
- Update atomically when revalidation completes
- Emit signal to trigger UI repaint

---

## Future Extensions

1. **Per-Aspect Memory Limits**: Config max cache size per aspect (e.g., 1GB preview, 5GB playback)
2. **Predictive Revalidation**: Prefetch playback for clips approaching playhead
3. **Aspect Priorities**: User control (e.g., "prioritize export quality over playback latency")
4. **Selective Invalidation**: Only invalidate changed region (e.g., clip resized 100ms, only tail invalidated)
5. **Aspect Aliases**: `Playback+Metadata` → `Realtime` for convenience

---

## References

- Unix page cache: Invalidate on write, validate on read
- Vulkan double-buffering: Different lifetime for frame data
- DAWs (Ableton, Logic): Lazy waveform generation, eager audio buffering
- Chromium blink: Invalidation sets dirty bits, validation on demand

