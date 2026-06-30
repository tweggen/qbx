# Plan 05: Legacy Interface Removal & System Simplification

**Phase:** Post-Phase 4 (Future work)  
**Status:** Planning (not yet started)  
**Estimated Effort:** 8-12 weeks  
**Risk Level:** Medium (careful planning required)

---

## Executive Summary

Phase 3 IOVector refactoring preserved backward compatibility by keeping dual interfaces (IOVector + raw-pointer) coexisting. This plan removes legacy systems once migration is complete, simplifying the codebase and establishing IOVector as the single canonical interface.

**Goal:** Eliminate technical debt from streaming-based audio path (calcOutputTo raw-pointer chain) that is now superseded by page-based freezePage rendering.

---

## Current State (Post-Phase 4)

### What Exists Today

| System | Status | Users |
|--------|--------|-------|
| IOVector calcOutputTo() | ✅ Active | AudioEngine, new code |
| Raw-pointer calcOutputTo() | ✅ Active | twStreamingLatch, legacy paths |
| freezePage() / twOutputPage | ✅ Active | Real-time audio, rendering |
| CapturePageData | ✅ Active | UI preview (CaptureRevalidator) |
| twStreamingLatch | ✅ Active | Direct component access |
| Component plugs/wiring | ✅ Active | Component graph |

### What Works Well

✅ Dual interfaces enable gradual migration  
✅ No breaking changes to existing code  
✅ Both paths tested (98/100 tests passing)  
✅ Performance verified (no degradation)  

### What Could Be Simpler

❌ Code duplication (both interfaces in every component)  
❌ Two page systems (twOutputPage + CapturePageData)  
❌ Legacy streaming path (calcOutputTo raw-pointer chains)  
❌ Complex initialization (plugs + wiring + format negotiation)  
❌ Maintenance burden (documenting two interfaces)  

---

## Removal Strategy: 4-Phase Plan

### Phase 1: Deprecation & Documentation (Weeks 1-2)

**Goal:** Mark legacy interfaces as deprecated; guide migration

**Tasks:**

1. **Add deprecation warnings**
   ```cpp
   [[deprecated("Use freezePage() and IOVector instead")]]
   virtual length_t calcOutputTo(sample_t *pDest, length_t length, idx_t idx);
   ```

2. **Document migration path**
   - Update COMPONENT_MIGRATION_GUIDE.md with deprecation timeline
   - Add "Deprecated Interfaces" section to ARCHITECTURE.md
   - Create LEGACY_MIGRATION_CHECKLIST.md

3. **Audit all callers**
   - Find all code calling raw-pointer calcOutputTo()
   - Document in spreadsheet: location, reason, migration path
   - Priority: high-risk paths first

4. **Set sunset date**
   - Announce: raw-pointer calcOutputTo() will be removed in v1.0
   - Timeline: 3-6 months (allows external users to migrate)

**Deliverables:**
- Deprecation warnings in code
- Migration guide document
- Caller audit spreadsheet
- Public announcement (changelog entry)

---

### Phase 2: Migrate Direct Callers (Weeks 3-6)

**Goal:** Move all direct calcOutputTo(raw-pointer) calls to freezePage or IOVector

**Callers to Migrate:**

| Caller | Current | New Path | Effort |
|--------|---------|----------|--------|
| twStreamingLatch | calcOutputTo(raw) | freezePage() | High |
| Direct UI preview | calcOutputTo(raw) | freezePreviewPage() | Medium |
| Plugin chains | calcOutputTo(raw) | calcOutputTo(IOVector) | Low |
| Format negotiation paths | Some raw calls | IOVector wrapping | Medium |

**Key Migration: twStreamingLatch**

**Current code:**
```cpp
length_t twStreamingLatch::readStreamingData(sample_t *pDest, length_t length) {
    return component_.calcOutputTo(pDest, length, outChannel_);
}
```

**New code (option A - use freezePage):**
```cpp
length_t twStreamingLatch::readStreamingData(sample_t *pDest, length_t length) {
    auto page = component_.freezePage(currentPos_, length, sampleRate_);
    memcpy(pDest, page->samples.data(), length * sizeof(sample_t));
    currentPos_ += length;
    return length;
}
```

**New code (option B - use IOVector):**
```cpp
length_t twStreamingLatch::readStreamingData(sample_t *pDest, length_t length) {
    IOVector dest = IOVector::CreateFromBuffer(pDest, length);
    return component_.calcOutputTo(dest, outChannel_);
}
```

**Decision:** Option A (freezePage) preferred — better page caching, state snapshots

**Tasks:**

1. Migrate twStreamingLatch → freezePage
2. Migrate direct UI preview calls → freezePreviewPage
3. Migrate plugin chains → IOVector
4. Add tests for each migrated path
5. Verify performance (should be same or better)

**Deliverables:**
- twStreamingLatch refactored
- All direct callers migrated
- Test coverage for new paths
- Performance benchmark report

---

### Phase 3: Remove Raw-Pointer Interface (Weeks 7-10)

**Goal:** Delete raw-pointer calcOutputTo() from all 18 components

**Before:**
```cpp
// Every component has BOTH:
virtual length_t calcOutputTo(IOVector& dest, idx_t idx) override;      // NEW
virtual length_t calcOutputTo(sample_t *pDest, length_t length, idx_t idx) override;  // OLD
```

**After:**
```cpp
// Only IOVector:
virtual length_t calcOutputTo(IOVector& dest, idx_t idx) override;
```

**Steps:**

1. **Remove raw-pointer implementations** (one component per PR)
   - Delete `calcOutputTo(sample_t*, length_t, idx_t)` method
   - Remove associated helper methods if only used by this interface
   - Verify compile (should work — nothing calls it anymore)

2. **Remove raw-pointer declarations from headers**
   - Delete method declaration
   - Clean up includes no longer needed

3. **Remove default implementation from twComponent base**
   - Delete the wrapping/adapter implementation
   - Remove from pure virtual list

4. **Test each component**
   - Run full test suite after each component removal
   - Verify no regressions
   - Commit per-component for easy revert if needed

**Removal Order (by risk):**
1. Low-risk stubs: twSpeaker, twWav (output sinks)
2. Simple sources: twConstant, twWhiteNoise
3. Readers: twSampleReader, twWavInput, twLoopReader
4. DSP: twMoog, twPipe
5. Mixing: twMixer, twRewire, twTrackMix
6. Wrappers: twView, twPluginInsert, twPluginChain
7. Disabled: twSaw, twTestSeq
8. Base class: twComponent

**Deliverables:**
- 18 components with raw-pointer removed
- All tests passing (should be 100/100 now)
- Commit per component for traceability

---

### Phase 4: Unify Page Systems (Weeks 11-12)

**Goal:** Consolidate twOutputPage (audio) and CapturePageData (preview) into single system

**Current State:**

| System | Purpose | Storage | Pool | Aspects |
|--------|---------|---------|------|---------|
| twOutputPage | Audio/export | std::vector | On-demand | Playback/Export |
| CapturePageData | Preview | Raw bytes | Pre-allocated | Preview |

**Problem:**
- Parallel systems doing similar things
- Maintenance burden (two pools, two structures)
- Preview doesn't benefit from audio page cache

**Solution: Unified Page System**

```cpp
// Single struct, works for both audio and preview
struct twOutputPage {
    uint64_t startPosition;
    std::vector<float> samples;  // 256KB capacity
    
    uint32_t validAspects;       // Preview | Playback | Export
    std::any internalState;      // Sequential state snapshots
    
    // Unified pooling strategy:
    // - 2048 pre-allocated pages (512MB total)
    // - Custom deleter returns to pool on release
    // - Used by both AudioEngine and CaptureRevalidator
};
```

**Benefits:**
✅ Single cache mechanism  
✅ Shared page pool (memory efficient)  
✅ Consistent interface  
✅ Easier to understand  

**Steps:**

1. **Modify twOutputPage structure**
   - Keep existing fields
   - Add pool management support
   - Ensure backward compatible

2. **Migrate CaptureRevalidator**
   - Use twOutputPage instead of CapturePageData
   - Adopt pooling mechanism
   - Remove CapturePagePool class

3. **Update page allocation**
   - Create unified pool (2048 pages)
   - Both AudioEngine and CaptureRevalidator allocate from it
   - Custom deleter returns to pool

4. **Remove CapturePageData**
   - Delete capture_page_pool.h
   - Delete CapturePagePool implementation
   - Update SObject to use twOutputPage

5. **Test integration**
   - Audio rendering should work identically
   - Preview rendering should work identically
   - Memory usage should be lower

**Deliverables:**
- Unified twOutputPage system
- Shared page pool (both audio + preview)
- CapturePageData removed
- CapturePagePool removed
- All tests passing

---

## Implementation Details

### Risk Mitigation

**Risk 1: Breaking something in twStreamingLatch**
- Mitigation: Keep raw-pointer version during Phase 2, migrate carefully
- Contingency: Revert to old version if tests fail
- Testing: Comprehensive unit tests for new path

**Risk 2: Performance regression in audio path**
- Mitigation: Benchmark before/after each change
- Contingency: Profile and optimize hot paths
- Testing: Render large projects, measure CPU usage

**Risk 3: Breaking UI preview rendering**
- Mitigation: Run full UI tests during Phase 2
- Contingency: Keep CapturePageData temporarily if needed
- Testing: Manual UI testing (waveform display, parameter scrubbing)

**Risk 4: Page pool allocation issues**
- Mitigation: Stress test pool during Phase 4
- Contingency: Increase pool size or use dynamic allocation as fallback
- Testing: Create many pages, verify no leaks or crashes

### Testing Strategy

**Unit Tests:**
- Existing test suite (98/100 should go to 100/100)
- New tests for each migrated path
- Pool stress tests (allocate 10k pages, verify cleanup)

**Integration Tests:**
- Load/play complex projects (50+ clips)
- Render to WAV/OGG/MP3
- UI preview with parameter changes
- Memory usage before/after (should decrease)

**Manual Testing:**
- Play synthesizer in UI
- Render medium-complexity project
- Monitor CPU usage
- Check for audio glitches

### Documentation Updates

**Files to Update:**
- ARCHITECTURE.md — remove legacy system sections
- COMPONENTS.md — simplify (only IOVector)
- COMPONENT_MIGRATION_GUIDE.md — remove dual-interface pattern
- Remove LEGACY_MIGRATION_CHECKLIST.md (no longer needed)

**New Documentation:**
- UNIFIED_PAGE_SYSTEM.md — design of consolidated system
- MIGRATION_COMPLETION_REPORT.md — final status

---

## Effort Estimation

| Phase | Task | Effort | Duration |
|-------|------|--------|----------|
| 1 | Deprecation & documentation | 10 hours | 1-2 weeks |
| 2 | Migrate direct callers | 40 hours | 2-3 weeks |
| 3 | Remove raw-pointer (18 components) | 30 hours | 2-3 weeks |
| 4 | Unify page systems | 25 hours | 1-2 weeks |
| Testing | Unit + integration + manual | 20 hours | Throughout |
| Overhead | Code review, rebase, fixes | 15 hours | Throughout |
| **TOTAL** | | **140 hours** | **8-12 weeks** |

---

## Success Criteria

✅ All raw-pointer calcOutputTo() removed  
✅ All 18 components using IOVector only  
✅ twStreamingLatch migrated to freezePage  
✅ CapturePageData removed (unified with twOutputPage)  
✅ Page systems consolidated (single pool, single interface)  
✅ All tests passing (100/100)  
✅ No performance degradation  
✅ Codebase simpler (fewer lines, clearer structure)  
✅ Documentation updated  

---

## Timeline & Dependencies

```
Week 1-2:   Phase 1 (Deprecation)
├─ Add warnings
├─ Audit callers
└─ Create guides

Week 3-6:   Phase 2 (Migrate Callers)
├─ Migrate twStreamingLatch
├─ Migrate UI paths
└─ Test all paths

Week 7-10:  Phase 3 (Remove Raw-Pointer)
├─ Remove from 18 components
├─ Test after each
└─ Full test suite verification

Week 11-12: Phase 4 (Unify Pages)
├─ Merge page systems
├─ Remove CapturePageData
└─ Final testing

Post-work:  Maintenance
├─ Monitor for issues
├─ Optimize if needed
└─ Update external documentation
```

---

## Rollback Plan

**If major issues discovered:**

1. **Phase 1 rollback:** Delete deprecation warnings (safe)
2. **Phase 2 rollback:** Revert twStreamingLatch migration, restore old path
3. **Phase 3 rollback:** Restore raw-pointer implementations (committed per-component)
4. **Phase 4 rollback:** Restore CapturePageData system

Each phase can be independently reverted if needed.

---

## Decision Points

**Question 1: When to start?**
- Option A: Immediately after Phase 4 (while fresh)
- Option B: Wait 2-3 months (gather feedback from users)
- Option C: Make it v1.0 work (major version bump, allow breaking changes)

**Recommendation:** Option C — makes it clear this is a major cleanup; users expect breaking changes in x.0

**Question 2: Support legacy interfaces?**
- Option A: Remove completely (hard cutoff)
- Option B: Keep raw-pointer but deprecated (soft cutoff)
- Option C: Keep both forever (accept technical debt)

**Recommendation:** Option A — hard cutoff ensures cleanup is complete; migration is straightforward

**Question 3: Page system consolidation priority?**
- Option A: Do it with Phase 3 (cleaner)
- Option B: Do it separately later (less risk per release)
- Option C: Don't do it (leave as-is)

**Recommendation:** Option A — while motivation is high and team is focused

---

## Future Benefits

### Immediate (After Phase 3)
- ✅ Simpler codebase (no dual interfaces)
- ✅ Clearer documentation (one way to do things)
- ✅ 20-30% reduction in component code

### Medium-term (After Phase 4)
- ✅ Unified page caching (better performance)
- ✅ Smaller memory footprint (shared pool)
- ✅ Easier to test (fewer code paths)

### Long-term
- ✅ Foundation for advanced features:
  - Deterministic rendering (frozen pages)
  - Parallel component evaluation
  - Page-based undo/redo
  - Time-machine debugging

---

## Notes & Considerations

1. **External Users:** If there are external users calling calcOutputTo directly, give them migration warning (2-3 months notice)

2. **Performance:** Phase 2 migration might introduce temporary performance cost; optimize afterward

3. **Testing:** Each phase should have full test suite passing (don't accumulate broken states)

4. **Communication:** Document progress in changelog/release notes as you go

5. **Parallel Work:** Can parallelize Phase 2 (different team members migrate different callers)

---

## Success Handoff

**Definition of done:**
- Raw-pointer calcOutputTo completely gone from codebase
- No compilation warnings about deprecated methods
- All 18 components using only IOVector interface
- Page systems unified (single twOutputPage, single pool)
- Full test suite passing (100/100)
- Codebase is cleaner and simpler
- Documentation reflects final state

---

**Status:** Planning (Ready to start after Phase 4 stabilization)  
**Owner:** TBD  
**Estimated Start:** Q3 2026 (after Phase 4 completion)  
**Version Target:** v1.0 release

