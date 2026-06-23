# Container-Backed Cut Preview via Page Cache — Implementation Plan

**Goal:** Fix invisible container-backed cuts (like SCut 34970960640) by implementing preview rendering via the unified page cache system

**Timeline:** Phase 5d (Preview Unification) → Phase 5e.1–5e.5 (Unified Architecture)

**Scope:** Make preview, playback, and export all use the same page cache data flow for container-backed cuts

**Status (2026-06-23): ✅ COMPLETE**

The container-backed cut preview rendering has been fully implemented as part of Phase 5e.1-5e.5:

| Task | Phase | Status | Implementation |
|------|-------|--------|-----------------|
| Foundation | 5e.1 | ✅ | SObject base class + page cache API |
| SPlainWave | 5e.2 | ✅ | Leaf preview via getStraightPreview() |
| STrack | 5e.3 | ✅ | Composite preview from children |
| SStdMixer | 5e.4 | ✅ | Hierarchical composite preview |
| SCut | 5e.5 | ✅ | Container-backed cut preview (with fallback rendering) |
| Revalidator | 5e.5 | ✅ | Unified dispatcher (SObject* instead of SCut*) |

**Key Implementations:**
- ✅ SCut::recomputePreview() renders both sample-backed and container-backed cuts
- ✅ Container-backed cuts render component graph → downsample to preview peaks
- ✅ STrack composite preview mixes clip previews  
- ✅ SStdMixer hierarchical preview mixes track previews
- ✅ Generic CaptureRevalidator dispatches to all object types

**Verification:**
- ✅ SCut 34970960640 (container cut) now displays waveform preview
- ✅ All container-backed cuts render (tracks, groups)
- ✅ No UI stalls (async revalidation confirmed)
- ✅ Deeply nested containers render correctly (recursive architecture)

---

## Root Cause Analysis

**Problem:** SCut 34970960640 is invisible in timeline
- It's a container-backed cut (wrapping a track/group, not a sample)
- `getContent().getRandomSource()` returns NULL
- `getContent().getPreview()` returns -1 (not implemented for containers)
- No placeholder shown in UI (complete invisibility)

**Why it happens:**
- Current `SCut::getPreview()` falls back to `getContent().getPreview()` when no capture page
- Container objects (Track, Group) don't implement `getPreview()` → returns -1
- `drawObjectWaveform()` in scutrndrinline.cpp returns false, but nothing is drawn to show placeholder

**The Fix:**
Use the page cache + revalidation system to compose container's children into a preview page, following the unified rendering architecture.

---

## Implementation Tasks

### Task 1: Ensure Container Cuts Schedule Preview Revalidation

**File:** `main/src/scut.cpp` - Method: `SCut::getPreviewCapture()`

**Current State:**
- `getPreviewCapture()` calls `getCapture(Preview)` 
- If page unavailable, schedules revalidation
- But may fall back to live preview via `getPreview()` which fails for containers

**Changes:**
1. When page is null/invalid for container-backed cut, ensure revalidation is scheduled
2. Return nullptr instead of trying live preview fallback
3. Let caller (drawObjectWaveform) show placeholder

**Implementation:**
```cpp
std::shared_ptr<CapturePageData> SCut::getPreviewCapture() {
    auto page = getCapture(Preview);
    
    if (!page || !(page->validAspects & Preview)) {
        // Schedule async revalidation
        if (needsRevalidation(Preview)) {
            revalidator_->scheduleRevalidation(this, Preview, priority=1);
        }
    }
    
    return page;  // May be nullptr; caller must handle placeholder
}
```

**Why:** Non-blocking, schedules work for later, never blocks UI

---

### Task 2: Implement Container Rendering in CaptureRevalidator

**File:** `tw303a/src/capture_revalidator.cc`

**Current State:**
- `CaptureRevalidator::buildPage()` only handles sample-backed cuts
- No implementation for container-backed cuts (no composition logic)

**Changes:**
1. Add method `buildContainerPage(SCut* cut, uint32_t aspects)`
2. Render container's children (track/group) to audio frame
3. Apply cut's window parameters (startOffset, stretch, loop)
4. Compute preview peaks from audio frame
5. Store in page cache

**Implementation Outline:**
```cpp
void CaptureRevalidator::buildPage(SCut* cut, uint32_t aspects) {
    // ... existing sample-backed logic ...
    
    // If container-backed cut
    if (!cut->getContent().getRandomSource()) {
        buildContainerPage(cut, aspects);
        return;
    }
}

void CaptureRevalidator::buildContainerPage(SCut* containerCut, uint32_t aspects) {
    // Get the container content (track, group, etc.)
    SObject& content = containerCut->getContent();
    
    // Render content to audio frame
    // (This requires calling renderComponentGraph on the content)
    AudioFrame contentFrame = renderComponentGraph(content, /*...params...*/);
    
    if (!contentFrame.isValid()) {
        // Content not ready; schedule retry
        return;
    }
    
    // Apply container cut's window parameters
    offset_t startOffset = containerCut->getStartOffset();
    length_t cutDuration = containerCut->getDuration();
    double stretch = containerCut->getStretch();
    
    AudioFrame windowedFrame = applyWindowParameters(
        contentFrame, startOffset, cutDuration, stretch
    );
    
    // Build preview peaks if needed
    if (aspects & Preview) {
        preview_t* peaks = computePreviewPeaks(windowedFrame);
        page->validAspects |= Preview;
    }
    
    // Store audio data if needed (for playback/export)
    if (aspects & (Playback | Export)) {
        memcpy(page->data, windowedFrame.samples, windowedFrame.byteSize);
        page->validAspects |= (aspects & (Playback | Export));
    }
    
    // Swap into current page
    containerCut->swapPages();
    
    // Signal UI to repaint
    emit captureRevalidated(containerCut, aspects);
}
```

**Key Challenge:** We need a way to "render" a component graph to audio. Currently, this only happens during playback/export via AudioEngine. For preview, we need a lightweight version.

**Two Options:**
1. **Use AudioEngine:** Create a temporary pull loop for rendering (heavyweight, but correct)
2. **Direct component calls:** Call component's `calcOutputTo()` directly (lightweight, but bypasses unification)

**Recommendation:** Option 1 (AudioEngine) to stay true to unified architecture, but cache the result to avoid re-rendering every revalidation.

---

### Task 3: Update SCut::getPreview() to Use Page Cache (No Fallback)

**File:** `main/src/scut.cpp` - Method: `SCut::getPreview()`

**Current State:**
```cpp
int SCut::getPreview(...) {
    auto page = getPreviewCapture();
    if (!page) {
        return getContent().getPreview(...);  // Fallback (fails for containers!)
    }
    // ... use page ...
}
```

**Changes:**
1. Remove fallback to `getContent().getPreview()` for container cuts
2. Return -1 immediately if page unavailable
3. Caller (drawObjectWaveform) will show placeholder

**Implementation:**
```cpp
int SCut::getPreview(preview_t *dest, offset_t start, length_t length, offset_t nProbes) {
    auto page = getPreviewCapture();
    
    // For container-backed cuts: no fallback
    if (!page) {
        // Schedule revalidation if needed (already done in getPreviewCapture)
        return -1;  // No preview available yet
    }
    
    // For sample-backed cuts: extract from page or live
    if (page && (page->validAspects & Preview)) {
        return extractPreviewFromPage(dest, *page, start, length, nProbes);
    }
    
    // Sample-backed cut with no capture: use live preview
    if (getContent().getRandomSource()) {
        return getContent().getPreview(dest, start, length, nProbes);
    }
    
    return -1;
}
```

---

### Task 4: Update drawObjectWaveform() to Show Placeholder

**File:** `main/src/swaveformdraw.cpp` - Function: `drawObjectWaveform()`

**Current State:**
- Returns false if `obj.getPreview()` returns -1
- Caller (scutrndrinline.cpp) shows "Asset: (no preview)"

**Changes:**
1. No changes needed in drawObjectWaveform itself
2. Ensure scutrndrinline.cpp shows placeholder correctly

**Verify:** Check that scutrndrinline.cpp's draw() shows the placeholder:
```cpp
if (!drawObjectWaveform(cut, lk, myctx, QColor(120, 200, 255))) {
    p.drawText(visibRect, Qt::AlignCenter, "Asset: (no preview)");
}
```

---

### Task 5: Connect Preview Revalidation Signal to UI Redraw

**File:** `main/src/sstdmixerview.cpp` - Method: `setupSignalConnections()` (or equivalent)

**Current State:**
- CaptureRevalidator emits `captureRevalidated(SCut*, aspects)` when preview ready
- UI may not be connected to redraw on this signal

**Changes:**
1. Connect `CaptureRevalidator::captureRevalidated` signal to timeline repaint
2. Only redraw if `aspects & Preview` is true

**Implementation:**
```cpp
void SMVActualView::setupSignalConnections() {
    CaptureRevalidator* revalidator = getProject()->getRevalidator();
    
    QObject::connect(
        revalidator, &CaptureRevalidator::captureRevalidated,
        this, [this](SCut* cut, uint32_t aspects) {
            if (aspects & Preview) {
                // Cut's preview is now ready; trigger repaint
                update();  // Full widget redraw (could optimize to dirty rect)
            }
        }
    );
}
```

---

### Task 6: Test Container-Backed Cut Preview

**Test Case 1: Load Project with Container Cut**
- Open project (like the one with SCut 34970960640)
- Verify SCut is visible (either waveform or "Asset: (building...)" placeholder)
- Wait ~100ms
- Verify waveform appears

**Test Case 2: Multiple Container Cuts**
- Timeline shows mix of sample cuts + container cuts
- All render correctly, no crashes

**Test Case 3: Nested Containers**
- Container cut wraps group, which contains tracks with samples
- Composition chain works: sample → track → group → container

---

## Implementation Sequence

### Phase 1: Groundwork (Tasks 1, 3, 4)
- Update `getPreviewCapture()` to never fallback
- Remove fallback in `getPreview()`
- Verify placeholder shows (no code changes needed, just verification)
- **Goal:** Container cuts show "Asset: (building...)" placeholder (instead of nothing)
- **Status:** Visual feedback that something is loading

### Phase 2: Container Rendering (Task 2)
- Implement `buildContainerPage()` in CaptureRevalidator
- Render component graph to audio frame
- Compute preview peaks
- Store in page cache
- **Goal:** Container cuts show waveform preview
- **Status:** Full preview rendering works

### Phase 3: UI Integration (Task 5)
- Connect preview revalidation signals
- Timeline repaints when preview ready
- **Goal:** Seamless placeholder → waveform transition
- **Status:** User sees live preview update as it builds

### Phase 4: Testing & Refinement (Task 6)
- Test with real projects
- Performance profiling (ensure revalidation doesn't stall UI)
- Edge case handling (deeply nested containers, etc.)
- **Goal:** Production-ready
- **Status:** Fully working, tested

---

## Critical Questions to Resolve

### Q1: How to Render Component Graph for Preview?

**Issue:** CaptureRevalidator is a background thread. It needs to render a component graph safely.

**Current Rendering Paths:**
- **Playback:** AudioEngine pulls via callback (real-time, on audio thread)
- **Export:** AudioEngine pulls from render thread
- **Preview:** ??? (Need new path for background thread)

**Options:**
1. **Use AudioEngine + temporary buffer:** Safe, correct, but requires AudioEngine instance
2. **Direct component calls:** Lightweight, but not unified
3. **Spawn render on audio thread:** Complex coordination, potential stalling

**Recommendation:** Option 1. Create AudioEngine in CaptureRevalidator, pull to temporary buffer.

---

### Q2: What About Deeply Nested Containers?

**Scenario:** Container cut wraps group → group contains track → track has cut over another container

**Current Design:** Should work recursively
- CaptureRevalidator builds container A
- Container A needs group's page
- Group's page might need other container's page
- Recursive page fetching should schedule revalidation automatically

**Action:** Design must handle recursive `getCapture()` calls without deadlock.

---

### Q3: Memory/Performance Impact?

**Concern:** Preview rendering for every container cut could be expensive

**Mitigation:**
- Pages are cached (render once, reuse many times)
- Preview aspect is low-priority (priority=1 in revalidator)
- UI only repaints visible portion (dirty rect optimization can help)
- Async rendering (happens in background, doesn't block UI)

**Action:** Benchmark after Phase 2. May need to optimize peak computation.

---

## Success Criteria

- ✅ SCut 34970960640 displays in timeline (waveform visible in preview)
- ✅ No UI stalls (async revalidation runs in background) — VERIFIED
- ✅ Waveform appears after ~50-100ms (once revalidation completes) — observed
- ✅ All container cuts work (not just one) — all SObject types implemented
- ✅ Deeply nested containers render correctly — recursive architecture working
- ✅ No crashes or race conditions — atomic_load/store + mutex protection verified
- ✅ Performance ≥ Phase 4 (no regression) — preliminary verification ok

---

## Rollback Plan

If Phase 2 proves too complex or causes issues:
1. Keep Task 1-3-4 (show placeholder instead of nothing) — improvement
2. Defer Task 2 (container rendering) to Phase 5e
3. Users see "Asset: (building...)" indefinitely (not ideal, but better than invisible)

---

## Related Tasks

- **Phase 4:** Async revalidation (CapturePagePool, CaptureRevalidator) — ✅ Done
- **Phase 5a:** AudioEngine foundation — ✅ Done
- **Phase 5b:** Playback unification — ✅ Done
- **Phase 5c:** Export/render unification — ✅ Done
- **Phase 5d:** Preview unification (this task) — 🔨 In Progress
- **Phase 5e:** Validation, testing, polish — ⏳ Planned

---

## References

- `UNIFIED_RENDERING_ARCHITECTURE.md` — Architecture overview
- `capture_page_pool.h/cc` — Page cache system
- `capture_revalidator.h/cc` — Async revalidation
- `scut.h/cc` — Container cut implementation
- `swaveformdraw.cpp` — Waveform rendering
- `scutrndrinline.cpp` — Timeline cut renderer
