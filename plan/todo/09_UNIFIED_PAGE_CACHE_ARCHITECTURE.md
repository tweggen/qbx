# Unified Page Cache Architecture for All SObjects

**Goal:** Extend the page cache + async revalidation system from SCut to all SObjects (STrack, SGroup, SStdMixer, SPlainWave), creating a uniform rendering pipeline.

**Scope:** All composite/container objects that aggregate content from children or compute rendered output.

**Timeline:** Phase 5e (Post-unification polish & extensibility)

**Implementation Status (2026-06-23):**

| Phase | Component | Status | Commit |
|-------|-----------|--------|--------|
| 5e.1 | SObject base class foundation | ✅ COMPLETE | 8cd4d69 |
| 5e.2 | SPlainWave preview caching | ✅ COMPLETE | e0dd0c8 |
| 5e.3 | STrack composite preview | ✅ COMPLETE | 22cfa42 |
| 5e.4 | SStdMixer hierarchical preview | ✅ COMPLETE | 8d41381 |
| 5e.5 | Unified CaptureRevalidator | ✅ COMPLETE | 982a5d6 |
| 5e.6 | Integration & testing | 🔄 IN PROGRESS | — |

**Verification Status:**
- ✅ Audible playback confirmed (macOS)
- ✅ Simple tracks play correctly
- ✅ Container-backed cuts render (previews now visible)
- ✅ Solo/Mute lag eliminated (Preview aspect now invalidated)
- ✅ UI updates working (background color refresh fixed)
- ⚠️ Container-backed cut looping incomplete (first iteration only)
- ⚠️ Group cut playback volume discrepancy (3-6dB over baseline)

---

## SObject Access Pattern Comparison

### Access Pattern Analysis

| SObject | Content Type | Access Pattern | Seek Capability | Notes |
|---------|-------------|-----------------|-----------------|-------|
| **SPlainWave** | Raw audio samples (file/memory buffer) | **Random** | Direct seek to any sample | Stateless; can read any region independently; used as leaf source |
| **SCut** | Reference to parent (SPlainWave, STrack, etc) with window parameters | **Random + Transform** | Seek with offset/stretch/loop applied | Transforms parent's output via window params; creates virtual "slice" |
| **STrack** | Ordered collection of SCuts on timeline | **Sequential at time T** | Position-based (mix all clips at time T) | Renders all visible clips at a given time; clips are ordered by start time |
| **SGroup** | Nested tracks (recursive) | **Hierarchical Sequential** | Position-based, recursive | Like STrack but children are STrack/SGroup instead of SCut; must recurse |
| **SStdMixer** | All tracks + master effects | **Hierarchical Sequential** | Position-based, recursive | Root of tree; renders all tracks at a given time; applies master gain/effects |

### Page Cache Applicability

All objects are **time-sliced** (pages represent ~0.68s windows at 48kHz):

| SObject | Page Relevance | Reasoning |
|---------|---|---|
| **SPlainWave** | ✅ **High** | Cache resampled/converted data. Random access within page. |
| **SCut** | ✅ **High** (already implemented) | Cache transformed output. Offset/stretch/loop applied to parent. |
| **STrack** | ✅ **High** | Cache mixed output of all clips at time T. Sequential iteration through clips. |
| **SGroup** | ✅ **High** | Cache recursive mix of child tracks. Hierarchical traversal. |
| **SStdMixer** | ✅ **High** | Cache final master output. All tracks composited + master effects. |

### Aspect Coverage

All objects produce the same four aspects:

| Aspect | SPlainWave | SCut | STrack | SGroup | SStdMixer |
|--------|---|---|---|---|---|
| **Preview** | Waveform peaks | ✅ | ✅ | ✅ | ✅ |
| **Playback** | Audio buffer | ✅ | ✅ | ✅ | ✅ |
| **Metadata** | Duration, RMS, peaks | ✅ | ✅ | ✅ | ✅ |
| **Export** | Resampled/normalized | ✅ | ✅ | ✅ | ✅ |

---

## Current State vs. Proposed

### Current (Fragmented)

```
SPlainWave
  └─ twRandomSource (twSampleReader with internal buffering)
     └─ No page cache
     └─ Live preview via getPreview() (blocking)

SCut
  └─ Page cache ✅ (implemented Phase 5d)
  └─ Async revalidation ✅
  └─ Thread-safe access ✅

STrack
  └─ No page cache
  └─ Renders clips live on demand
  └─ Each clip's preview fetched independently

SGroup
  └─ No page cache
  └─ Recursively renders children live

SStdMixer
  └─ No page cache
  └─ Renders all tracks live
```

### Proposed (Unified)

```
SObject (base class)
  ├─ currentPage_, nextPage_ ✅
  ├─ getCapture(aspects) ✅
  ├─ scheduleRevalidation() ✅
  ├─ atomic_load/store for pages ✅
  ├─ pageMutex in CapturePageData ✅
  └─ Virtual recomputePreview(), recomputePlayback(), etc.

SPlainWave extends SObject
  └─ recomputePreview(): Resample buffer to peaks
  └─ recomputePlayback(): Read samples from file/memory

SCut extends SObject
  └─ recomputePreview(): Apply window params to parent
  └─ recomputePlayback(): Apply grain params to parent

STrack extends SObject
  └─ recomputePreview(): Mix preview of all child clips
  └─ recomputePlayback(): Mix playback of all child clips

SGroup extends STrack
  └─ recomputePreview(): Recursively mix child groups
  └─ recomputePlayback(): Recursively mix child groups

SStdMixer extends SGroup
  └─ recomputePreview(): Apply master effects to final mix
  └─ recomputePlayback(): Apply master effects to final mix
```

---

## Implementation Phases

### Phase 5e.1: Refactor SObject Base Class (Foundation)

**Goal:** Move page cache infrastructure to SObject

**Changes:**
1. Move to `SObject`:
   - `currentPage_`, `nextPage_` member variables
   - `getCapture(uint32_t aspects)` method
   - `currentPage()` atomic-safe accessor
   - `swapPages_nolock()` atomic swap
   - `revalidator_` pointer

2. Create abstract interface:
   ```cpp
   class SObject {
   protected:
       virtual void recomputePreview(CapturePageData& page, uint32_t aspectsMask) = 0;
       virtual void recomputePlayback(CapturePageData& page, uint32_t aspectsMask) = 0;
       virtual void recomputeMetadata(CapturePageData& page, uint32_t aspectsMask) = 0;
       virtual void recomputeExport(CapturePageData& page, uint32_t aspectsMask) = 0;
   };
   ```

3. Remove from SCut:
   - Move currentPage_, nextPage_ definitions to base
   - Keep SCut-specific logic in recompute methods

**Files affected:**
- `main/include/sobject.h` — add page cache members & methods
- `main/src/sobject.cpp` — implement getCapture()
- `main/include/scut.h` — remove page members, keep recompute methods
- `main/src/scut.cpp` — minimal changes (methods stay)

**Testing:**
- SCut still renders correctly with inherited page cache
- No behavioral change (just code reorganization)

---

### Phase 5e.2: Implement SPlainWave Revalidation

**Goal:** Cache resampled waveform data for SPlainWave

**Current state:**
- Uses twRandomSource for live preview
- No caching, blocking preview calls

**Changes:**
1. Implement recomputePreview():
   ```cpp
   void SPlainWave::recomputePreview(CapturePageData& page, uint32_t aspects) {
       if (!(aspects & Preview)) return;
       
       // Get waveform via existing twRandomSource
       // Downsample to preview_t peaks (same as SCut)
       // Store in page.data
       
       page.validAspects |= Preview;
   }
   ```

2. Update getPreview() to use page cache:
   - First check `getCapture(Preview)`
   - Fall back to live preview only if page unavailable

**Benefits:**
- Smooth preview loading (shows stale data while revalidating)
- Scales to large files (only visible portion cached)

**Files affected:**
- `main/include/splainwave.h` — declare recompute methods
- `main/src/splainwave.cpp` — implement recompute methods

**Testing:**
- Load large audio file
- Verify preview shows while revalidation builds in background
- Seek around file, verify cache persists

---

### Phase 5e.3: Implement STrack Revalidation

**Goal:** Cache composite preview/playback of all clips

**Current state:**
- Renders clips live on demand
- No caching

**Changes:**
1. Implement recomputePreview():
   ```cpp
   void STrack::recomputePreview(CapturePageData& page, uint32_t aspects) {
       if (!(aspects & Preview)) return;
       
       // Get visible clips at this time
       for (SLink* clipLink : visibleClips(pageTime)) {
           SCut* clip = dynamic_cast<SCut*>(&clipLink->getSObject());
           if (!clip) continue;
           
           // Get clip's preview (triggers revalidation if needed)
           auto clipPage = clip->getCapture(Preview);
           if (!clipPage) continue;
           
           // Mix/composite clip preview into page
           mixPreviewIntoPage(page, *clipPage, clipLink->getStartTime());
       }
       
       page.validAspects |= Preview;
   }
   ```

2. Similar for recomputePlayback() — composite audio output

**Key challenges:**
- Determining which clips are "visible" in this time slice
- Mixing/compositing multiple clip previews into one page
- Handling variable clip positions and durations

**Files affected:**
- `main/include/strack.h` — declare recompute methods
- `main/src/strack.cpp` — implement recompute methods

**Testing:**
- Timeline with multiple clips
- Verify composite preview appears
- Add/remove clips, verify cache invalidation
- Reposition clips, verify preview updates

---

### Phase 5e.4: Implement SGroup / SStdMixer Revalidation

**Goal:** Recursive hierarchical rendering

**Changes:**
1. SGroup extends STrack, but children are STrack/SGroup instead of SCut
   ```cpp
   void SGroup::recomputePreview(CapturePageData& page, uint32_t aspects) {
       if (!(aspects & Preview)) return;
       
       for (SLink* trackLink : children_) {
           STrack* track = dynamic_cast<STrack*>(&trackLink->getSObject());
           if (!track) continue;
           
           auto trackPage = track->getCapture(Preview);  // Recursive!
           if (!trackPage) continue;
           
           mixPreviewIntoPage(page, *trackPage, ...);
       }
       
       page.validAspects |= Preview;
   }
   ```

2. SStdMixer is the root; renders all tracks + applies master effects

**Key benefit:** Truly hierarchical — groups can nest arbitrarily deep

**Files affected:**
- `main/include/sgroup.h` — if it exists
- `main/src/sgroup.cpp`
- `main/include/sstdmixer.h`
- `main/src/sstdmixer.cpp`

**Testing:**
- Nested groups (group within group)
- Deep hierarchies (3+ levels)
- Verify preview builds correctly at each level
- Master effects applied at top level

---

### Phase 5e.5: Unify CaptureRevalidator

**Goal:** Single worker pool for all SObject types

**Current state:**
- CaptureRevalidator knows about SCut

**Changes:**
1. Make revalidator polymorphic over SObject:
   ```cpp
   struct CaptureRevalidationJob {
       SObject* object;  // Not SCut* — any SObject
       uint32_t aspects;
       int priority;
   };
   ```

2. processJob() calls object->recomputePreview(), etc. (polymorphic)

3. No CaptureRevalidator changes needed for basic polymorphism
   - Just change SCut* to SObject*
   - All recompute logic in subclasses

**Files affected:**
- `tw303a/include/capture_revalidator.h` — change SCut* to SObject*
- `tw303a/src/capture_revalidator.cc` — minimal changes

**Testing:**
- UI paints timeline with all object types
- Revalidator jobs queued for SPlainWave, SCut, STrack, etc.
- Worker pool handles them uniformly

---

### Phase 5e.6: Integration & Testing

**Goal:** End-to-end validation

**Changes:**
1. Hook up UI to use new caching:
   - Remove old live preview calls where possible
   - Use getCapture() + display placeholder during revalidation

2. Performance profiling:
   - Measure cache hit rates
   - Verify no regressions vs. old system
   - Check worker thread CPU usage

3. Edge case testing:
   - Empty tracks
   - Very deep nesting
   - Rapid seeking (cache thrashing)
   - Large files + many clips

**Files affected:**
- UI rendering code (wherever getPreview() is called)
- Performance monitoring

---

## Success Criteria

- ✅ All SObject types use uniform page cache (Phase 5e.1 done)
- ✅ Single CaptureRevalidator pool for all objects (Phase 5e.5 done)
- ✅ Preview/playback/export all flow through same pipeline (Phase 5e.5 done)
- ✅ No UI stalls (fire-and-forget async model throughout) — VERIFIED ✅
- ✅ Thread-safe everywhere (atomic_load/store, pageMutex) — VERIFIED ✅
- ✅ _nolock convention enforced across codebase
- ✅ Performance >= current system (no regression) — preliminary verification ✅
- ⚠️ Nested groups render correctly (recursive composition) — rendering works, looping issue found

---

## Risk Assessment

| Risk | Severity | Mitigation |
|------|----------|-----------|
| **Large refactoring** | High | Phase it: base class → individual types → integration |
| **Breaking existing code** | Medium | Keep old methods as stubs during transition |
| **Performance regression** | Medium | Benchmark each phase; revert if needed |
| **Nested recursion deadlock** | Medium | Test deep hierarchies; add recursion limits if needed |
| **Page pool exhaustion** | Low | Monitor allocation; tune pool size if needed |

---

## Related Work

- **Phase 4:** Async revalidation foundation (done ✅)
- **Phase 5a–5c:** Audio engine unification (done ✅)
- **Phase 5d:** Container preview via page cache (done ✅)
- **Phase 5e.1–5e.6:** This work (unified architecture)
- **Phase 6:** Polish, optimization, CI/CD

---

## Rollback Plan

If issues arise at any phase:
1. **Phase 5e.1:** Revert SObject changes; keep SCut's own cache
2. **Phase 5e.2–5e.4:** Disable new recomputePreview() methods; revert to live preview
3. **Phase 5e.5:** Keep revalidator job as `SCut*`; don't polymorphize
4. **Phase 5e.6:** Remove new UI code; use old preview paths

Each phase is independently revertible.

---

## Notes

- The page size (256 KB) is shared across all object types
  - May need tuning based on typical preview resolution
  - Consider: larger pages for composite objects, smaller for leaves?
- Aspect masks (Preview, Playback, Metadata, Export) are universal
  - All objects produce the same aspects
  - Revalidator treats all aspects uniformly
- Recursive composition (SGroup → SGroup) requires careful thought
  - Ensure no infinite loops (circular dependencies)
  - Timeout/recursion depth limits may be needed
