# Async Revalidation Integration — Phase 4

**Status:** Design (Ready for implementation)  
**Author:** Timo Weggen, Claude  
**Date:** 2026-06-22  
**Scope:** Replace eager `ensureCapture()` with async two-page buffer model

## Overview

Integrate the `CapturePagePool` and `CaptureRevalidator` into the existing codebase:
- Pre-allocate 512MB-1GB pool (2048 × 256kB pages)
- Spawn 8 worker threads (aggressive testing)
- Replace blocking `ensureCapture()` with non-blocking `getCapture()`
- Update all call sites (playback, timeline painting, etc.)
- Maintain stale-data fallback (zero audio dropouts)

## Architecture Changes

### New Classes (in tw303a/)

```
tw303a/include/capture_page_pool.h        // CapturePagePool, CapturePageData
tw303a/src/capture_page_pool.cc           // Implementation
tw303a/include/capture_revalidator.h      // CaptureRevalidator, Job queue
tw303a/src/capture_revalidator.cc         // Worker threads, async revalidation
```

### Modified Classes

```
main/include/sobject.h                    // Add protected mutex() accessor
main/src/sobject.cpp                      // Implementation
main/include/scut.h                       // Two-page buffer, getCapture(), invalidateCapture()
main/src/scut.cpp                         // Async integration
main/include/sproject.h                   // Own CaptureRevalidator + CapturePagePool
main/src/sproject.cpp                     // Initialize pools/workers
```

---

## Detailed Integration Points

### 1. SObject Base Class: Add Protected Mutex

**File:** `main/include/sobject.h` / `main/src/sobject.cpp`

**Change:** Add mutex management to base class.

```cpp
class SObject : public QObject {
private:
    mutable std::mutex stateMutex_;
    
protected:
    // All subclasses use this to protect their state
    std::mutex& mutex() const {
        return stateMutex_;
    }
};
```

**Impact:** Every SObject now has built-in thread safety. SCut no longer needs its own mutex.

---

### 2. SCut: Two-Page Buffer Integration

**File:** `main/include/scut.h` / `main/src/scut.cpp`

#### Constructor/Destructor

```cpp
class SCut : public SObject {
private:
    CaptureRevalidator* revalidator_ = nullptr;  // Borrowed from SProject
    std::shared_ptr<CapturePageData> currentPage_;
    std::shared_ptr<CapturePageData> nextPage_;
    
public:
    SCut(SProject* parentProject, SObject& content);
    SCut(SProject* parentProject, SLink& content);
    virtual ~SCut();
};
```

**Implementation:**

```cpp
SCut::SCut(SProject* parentProject, SLink& content)
    : SObject(parentProject), content_(&content) {
    
    // Get revalidator from project
    revalidator_ = parentProject->getRevalidator();
    Q_ASSERT(revalidator_);
    
    // Register this cut as dependent of content object
    // When content changes (mute, solo, etc.), we're notified
    content_->getSObject().addDependentLink(content_);
    
    // Connect revalidator signals for UI updates
    QObject::connect(revalidator_, &CaptureRevalidator::captureRevalidated,
                     this, &SCut::onCaptureRevalidated);
}

SCut::~SCut() {
    // Qt's destroyed() signal auto-unregisters us from dependents
    // No manual cleanup needed
}
```

#### New Methods

```cpp
class SCut : public SObject {
public:
    // Non-blocking: get current/stale page (never waits)
    // Schedules async revalidation if needed
    std::shared_ptr<CapturePageData> getCapture(uint32_t aspectsMask);
    
    // Specific accessors for common paths
    std::shared_ptr<CapturePageData> getPlaybackCapture() {
        return getCapture(Playback | Metadata);
    }
    
    std::shared_ptr<CapturePageData> getPreviewCapture() {
        return getCapture(Preview);
    }
    
    // Check if revalidation needed (for diagnostics)
    bool needsRevalidation(uint32_t aspectsMask) const;
    
    // Internal: called by revalidator after swap
    void onCaptureRevalidated(uint32_t aspects) {
        // Emit signal for UI redraw if Preview updated
        if (aspects & Preview) {
            emit capturePreviewUpdated();
        }
    }

private:
    std::shared_ptr<CapturePageData> readLockCurrentPage() const {
        std::lock_guard lock(mutex());
        return currentPage_;
    }
    
    void swapPages() {
        std::lock_guard lock(mutex());
        std::swap(currentPage_, nextPage_);
        nextPage_ = nullptr;
    }
};
```

#### Updated `invalidateCapture()`

**Old behavior (blocking):**
```cpp
void SCut::invalidateCapture() {
    capture_.reset();
    rebuildReader();  // Eager!
    invalidateAspects(Preview | Playback | Metadata);
}
```

**New behavior (async):**
```cpp
void SCut::invalidateCapture() {
    {
        std::lock_guard lock(mutex());
        // Drop stale pages; next access triggers revalidation
        currentPage_.reset();
        nextPage_.reset();
    }
    
    // Schedule full revalidation (high priority if arrangements changed)
    revalidator_->scheduleRevalidation(this, Preview | Playback | Metadata | Export, 5);
}
```

#### Updated `invalidateAspects()` (from Phase 3)

```cpp
void SCut::invalidateAspects(uint32_t aspects) {
    {
        std::lock_guard lock(mutex());
        
        // Drop stale capture if Playback invalidated
        if (aspects & Playback) {
            currentPage_.reset();
        }
        
        // Mark bits invalid by scheduling revalidation
        if (currentPage_) {
            currentPage_->validAspects &= ~aspects;
        }
    }
    
    // Determine priority
    int priority = 5;  // Default: Metadata
    if (aspects & Playback) priority = 10;  // High: audio needs this
    if (aspects & Preview)  priority = 1;   // Low: UI can tolerate stale
    
    // Schedule async revalidation (outside lock)
    revalidator_->scheduleRevalidation(this, aspects, priority);
    
    // Propagate to dependents (transitive)
    if (aspects & Playback) {
        notifyDependentsChanged(Playback | Metadata);
    }
}
```

#### New `getCapture()` Method

```cpp
std::shared_ptr<CapturePageData> SCut::getCapture(uint32_t aspectsMask) {
    // Get current page (may be stale, may be null, never blocks)
    auto page = readLockCurrentPage();
    
    // If all needed aspects are valid, return immediately
    if (page && (page->validAspects & aspectsMask) == aspectsMask) {
        return page;
    }
    
    // Otherwise, schedule async revalidation (returns immediately)
    int priority = 5;
    if (aspectsMask & Playback) priority = 10;
    if (aspectsMask & Preview)  priority = 1;
    
    if (needsRevalidation(aspectsMask)) {
        revalidator_->scheduleRevalidation(this, aspectsMask, priority);
    }
    
    // Return current page (stale is OK; better than null/dropout)
    return page;
}

bool SCut::needsRevalidation(uint32_t aspectsMask) const {
    std::lock_guard lock(mutex());
    return !currentPage_ || (currentPage_->validAspects & aspectsMask) != aspectsMask;
}
```

---

### 3. SProject: Manage Pools and Revalidator

**File:** `main/include/sproject.h` / `main/src/sproject.cpp`

```cpp
class SProject : public SObject {
private:
    std::unique_ptr<CapturePagePool> pagePool_;
    std::unique_ptr<CaptureRevalidator> revalidator_;
    
public:
    SProject(const QString& name);
    virtual ~SProject();
    
    CaptureRevalidator* getRevalidator() { return revalidator_.get(); }
    CapturePagePool* getPagePool() { return pagePool_.get(); }
};
```

**Implementation:**

```cpp
SProject::SProject(const QString& name)
    : SObject(getApplicationObject()) {
    
    // Initialize 512MB pool (2048 pages × 256kB)
    // This is per-project, so multiple projects have separate pools
    pagePool_ = std::make_unique<CapturePagePool>(2048);
    
    // Spawn 8 worker threads (for stress testing; can dial back later)
    revalidator_ = std::make_unique<CaptureRevalidator>(pagePool_.get(), 8);
}

SProject::~SProject() {
    // Revalidator destructor gracefully shuts down workers
    revalidator_.reset();
    pagePool_.reset();
}
```

---

### 4. Update Call Sites: Audio Playback

**File:** `tw303a/src/twspeaker.cc` (or wherever playback reads cuts)

**Old code (blocking):**
```cpp
void twSpeaker::renderFrame(float* buffer, int frameCount) {
    for (auto cut : activeCuts) {
        // Blocking! May wait 50-100ms for revalidation
        cut->ensureCapture(Playback);
        
        twSampleReader* reader = cut->getReader();
        if (!reader) continue;
        
        reader->readFrames(buffer, frameCount);
    }
}
```

**New code (non-blocking):**
```cpp
void twSpeaker::renderFrame(float* buffer, int frameCount) {
    for (auto cut : activeCuts) {
        // Non-blocking: schedules if needed, returns current/stale
        auto page = cut->getPlaybackCapture();
        
        // If no page available (rare), fallback to silence
        if (!page) {
            writeZeros(buffer, frameCount);
            continue;
        }
        
        // Reconstruct reader from page (or use cached reader reference)
        twSampleReader* reader = reconstructReaderFromPage(*page);
        if (!reader) {
            writeZeros(buffer, frameCount);
            continue;
        }
        
        // Use reader (may be slightly stale, but audio never drops)
        reader->readFrames(buffer, frameCount);
    }
}
```

---

### 5. Update Call Sites: Timeline Painting

**File:** `main/src/smvactualview.cpp` (or wherever timeline renders)

**Old code (blocking):**
```cpp
void SMVActualView::paintEvent(QPaintEvent* event) {
    for (auto cut : visibleCuts) {
        // Blocking! May stall UI by 10-50ms per cut
        cut->ensureCapture(Preview);
        
        const preview_t* peaks = cut->getPreview();
        if (peaks) {
            drawWaveform(peaks, cut->getDuration());
        }
    }
}
```

**New code (non-blocking):**
```cpp
void SMVActualView::paintEvent(QPaintEvent* event) {
    for (auto cut : visibleCuts) {
        // Non-blocking: schedules if needed, returns current/stale
        auto page = cut->getPreviewCapture();
        
        if (!page) {
            drawPlaceholder(cut);  // Brief placeholder
            continue;
        }
        
        // Extract preview data from page
        const preview_t* peaks = reinterpret_cast<preview_t*>(page->data);
        drawWaveform(peaks, cut->getDuration());
    }
}

// Connect to revalidator signal to trigger redraw when Preview ready
void SMVActualView::setupSignalConnections() {
    CaptureRevalidator* revalidator = getProject()->getRevalidator();
    
    QObject::connect(revalidator, &CaptureRevalidator::captureRevalidated,
                     this, [this](SCut* cut, uint32_t aspects) {
        if (aspects & Preview) {
            // Cut's preview updated; trigger repaint
            update();  // Full widget repaint (could optimize to dirty rect)
        }
    });
}
```

---

### 6. Update Call Sites: Waveform Preview in Dialogs

**File:** `main/src/scut.cpp` - `getPreview()` accessor

**Old code (blocking):**
```cpp
int SCut::getPreview(preview_t* dest, offset_t start, length_t length, offset_t nProbes) {
    // Blocking!
    ensureCapture(Preview);
    
    if (!capture_) return -1;  // No capture
    
    // Extract preview from capture
    return extractPreview(dest, start, length, nProbes);
}
```

**New code (non-blocking):**
```cpp
int SCut::getPreview(preview_t* dest, offset_t start, length_t length, offset_t nProbes) {
    // Non-blocking: get current/stale page
    auto page = getPreviewCapture();
    
    if (!page) {
        // No page available; schedule and return error (caller retries on signal)
        return -1;
    }
    
    // Extract preview from page
    return extractPreviewFromPage(dest, *page, start, length, nProbes);
}
```

---

### 7. Render/Export Paths

**File:** `tw303a/src/render_session.cc`

**Old code:**
```cpp
void RenderSession::renderFrame() {
    // Blocking on export! May wait for revalidation
    cut->ensureCapture(Export | Playback);
    
    auto reader = cut->getReader();
    // ... render
}
```

**New code:**
```cpp
void RenderSession::renderFrame() {
    // Non-blocking: get Export + Playback (stale OK for export)
    auto page = cut->getCapture(Export | Playback);
    
    if (!page) {
        // Rare: export buffer not yet ready, wait briefly
        // (Or queue for retry; export is user-initiated, not time-critical)
        std::this_thread::sleep_for(10ms);
        page = cut->getCapture(Export | Playback);
    }
    
    if (!page) {
        // Still not ready; skip frame or write silence
        writeSilence(buffer);
        return;
    }
    
    auto reader = reconstructReaderFromPage(*page);
    // ... render
}
```

---

## Files to Modify (Summary)

| File | Changes |
|------|---------|
| `tw303a/include/capture_page_pool.h` | **NEW** — CapturePageData, CapturePagePool |
| `tw303a/src/capture_page_pool.cc` | **NEW** — Pool implementation |
| `tw303a/include/capture_revalidator.h` | **NEW** — CaptureRevalidator, Job queue |
| `tw303a/src/capture_revalidator.cc` | **NEW** — Worker threads, async revalidation |
| `main/include/sobject.h` | Add `protected std::mutex& mutex()` |
| `main/src/sobject.cpp` | Initialize `stateMutex_` |
| `main/include/scut.h` | Two-page buffer, `getCapture()`, update methods |
| `main/src/scut.cpp` | Implement new methods, update `invalidateCapture()`, `invalidateAspects()` |
| `main/include/sproject.h` | Add `pagePool_`, `revalidator_` members |
| `main/src/sproject.cpp` | Initialize pools/workers in ctor/dtor |
| `tw303a/src/twspeaker.cc` | Update playback to use `getPlaybackCapture()` |
| `main/src/smvactualview.cpp` | Update timeline painting to use `getPreviewCapture()` |
| `tw303a/src/render_session.cc` | Update export to use `getCapture()` |
| `main/src/scut.cpp` | Update `getPreview()` accessor |

---

## Testing Strategy

### Phase 1: Pool & Revalidator Basics
- [ ] `CapturePagePool` allocates/deallocates correctly
- [ ] `CaptureRevalidator` workers start and shutdown gracefully
- [ ] Job queue processes priority correctly

### Phase 2: Two-Page Integration
- [ ] SCut's `currentPage_` / `nextPage_` swap atomically
- [ ] Readers get consistent data during swap
- [ ] Stale data fallback works

### Phase 3: Call Site Updates
- [ ] Audio doesn't stall (playback uses `getPlaybackCapture()`)
- [ ] UI doesn't stall (timeline uses `getPreviewCapture()`)
- [ ] Revalidation signals trigger redraws

### Phase 4: Stress Testing (8 workers)
- [ ] No race conditions with 8 concurrent workers
- [ ] No use-after-free (pool management safe)
- [ ] Memory usage stays within bounds

---

## Rollback Plan

If async model causes issues:
1. Reduce worker count from 8 → 2 (simpler concurrency)
2. Reduce pool size (512MB → 256MB)
3. Add synchronous fallback: if page not ready after timeout, build sync
4. Revert to eager model (last resort)

---

## Performance Expectations

| Metric | Before | After |
|--------|--------|-------|
| Mute toggle hang | 1-2s | 0ms (async in background) |
| First audio frame after mute | Immediate (late) | Immediate (stale but audio OK) |
| Audio updates with mute | ~1-2s | ~50-100ms (lazy) |
| Timeline paint stall | 10-50ms per cut | 0ms (async) |
| Memory footprint | Minimal | +512MB-1GB (pools) |
| CPU (idle) | Minimal | 8 worker threads (negligible when idle) |

---

## Future Optimizations

- [ ] Dynamic worker pool size (scale by CPU cores)
- [ ] Per-project pool sharing (reduce memory in multi-project workflow)
- [ ] Priority inheritance (audio thread priority propagates to revalidator)
- [ ] Predictive prefetch (revalidate cuts approaching playhead)
- [ ] Memory limits per aspect (LRU eviction if pool fills)
