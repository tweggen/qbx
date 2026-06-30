# Phase 4: Page System Unification & Legacy Cleanup

**Status:** In Progress  
**Date Started:** 2026-06-30  
**Target Completion:** Session 4-5

---

## Overview

Phase 3 completed the raw-pointer interface removal across all 18 components, establishing IOVector as the primary interface. Phase 4 consolidates the rendering infrastructure by unifying the page systems and eliminating legacy rendering code paths that are no longer needed.

---

## Current State (Post-Phase 3)

### Two Separate Page Systems

**System 1: twOutputPage (Audio Rendering)**
- Used by: `freezePage()` in twComponent, AudioEngine pipeline
- Location: `tw303a/include/twoutputpage.h`
- Data: `std::vector<sample_t> samples`, `uint64_t startPosition`, `length_t validFrames`
- Purpose: Immutable pages for audio rendering

**System 2: CapturePageData (Preview)**
- Used by: `CaptureRevalidator`, SObject preview rendering
- Location: `tw303a/include/capture_revalidator.h`
- Data: Similar structure but separate type
- Purpose: Preview-time page caching

**Legacy System 3: renderObjectInto() (Abandoned)**
- Location: `scut.cpp:219-373`
- Purpose: Offline recursive renderer for container/grained cuts
- Status: Replaced by page-based rendering but code still present

### Identified Cleanup Opportunities

1. **renderObjectInto()** - Dead code, can be removed entirely
2. **buildCapture_()** - Synchronous capture builder that was replaced by revalidator
3. **SCut shadow page fields** - Duplicated fields causing confusion (SObject + SCut both declare currentPage_)
4. **Legacy recomputePlayback()** - Empty base method, called nowhere

---

## Phase 4 Plan

### Phase 4.1: Consolidate Page Systems (This Session)

**Goal:** Unify twOutputPage and CapturePageData into a single PageData type used by all rendering paths.

**Tasks:**
1. Define unified `PageData` struct combining twOutputPage and CapturePageData
2. Update freezePage() to use unified PageData
3. Update freezePreviewPage() to use unified PageData
4. Update CaptureRevalidator to use unified PageData
5. Remove old twOutputPage and CapturePageData definitions
6. Build verification after each task

**Deliverables:**
- ✅ Single page type used across audio and preview rendering
- ✅ Zero regressions (39/41 test baseline maintained)
- ✅ All components building cleanly

**Estimated Time:** 1-2 hours

---

### Phase 4.2: Remove Legacy Rendering Paths (Next Session)

**Goal:** Clean up dead code and abandoned rendering infrastructure.

**Tasks:**
1. Remove renderObjectInto() from scut.cpp
2. Remove buildCapture_() method
3. Remove SCut shadow page fields (use SObject's canonical fields only)
4. Remove legacy recomputePlayback() base method
5. Clean up any remaining legacy preview machinery
6. Update tests if needed

**Deliverables:**
- ✅ Cleaner codebase with dead code removed
- ✅ No reference ambiguity (single canonical page fields)
- ✅ Tests still pass

**Estimated Time:** 1-2 hours

---

## Optimization (Deferred to Backlog)

The following optimization opportunities are identified but deferred:
- Page pool allocation tuning (currently 2048 pages)
- Worker thread count optimization (currently 8)
- Cache locality improvements
- Memory usage profiling

These can be tackled in Phase 5+ once the architecture is fully unified and stable.

---

## Success Criteria

- ✅ Single unified page system used across all rendering paths
- ✅ No legacy code paths remaining (renderObjectInto, buildCapture_, etc.)
- ✅ Build clean with zero warnings
- ✅ Tests pass: 39/41 (baseline maintained)
- ✅ Git history clean with incremental commits

---

## Risk Mitigation

1. **Large file changes:** Do page consolidation incrementally (one method at a time)
2. **Regression:** Run tests after each task, build after each edit
3. **Breaking changes:** Preview rendering is well-tested, verify no regressions there
4. **Interdependencies:** twOutputPage and CapturePageData are used in multiple places; careful tracing needed

---

## Related Documentation

- **Phase 3:** `PHASE3_SESSION_NOTES.md` - Raw-pointer interface removal (COMPLETE)
- **Deferred:** `PLAN.md` - twTrackMix decoupling (implemented in Phase 3 work)
- **Architecture:** See `FREEZING_WIRES_ARCHITECTURE.md` for page-based rendering design
