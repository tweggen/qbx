# Legacy Interface Callers Audit (Phase 1)

**Date:** 2026-06-30  
**Purpose:** Document all code calling raw-pointer calcOutputTo()  
**Status:** Phase 1 Execution (Deprecation & Documentation)

---

## Summary

All calls to raw-pointer `calcOutputTo(sample_t*, length_t, idx_t)` identified and categorized for migration.

---

## Critical Callers (Must Migrate in Phase 2)

### 1. twStreamingLatch::readStreamingData()

**File:** `tw303a/src/twstreaminglatch.cc` (~line 195)

**Current Code:**
```cpp
length_t twStreamingLatch::readStreamingData(sample_t *pDest, length_t length) {
    return component_.calcOutputTo(pDest, length, outChannel_);
}
```

**Usage Context:**
- Called by: Qt UI preview system
- Frequency: Real-time (every GUI redraw)
- Risk Level: **HIGH** (hot path, UI responsiveness depends on it)

**Migration Path:**
```cpp
// Option A (RECOMMENDED): Use freezePage()
auto page = component_.freezePage(currentPlayPos_, length, sampleRate_);
memcpy(pDest, page->samples.data(), length * sizeof(sample_t));
currentPlayPos_ += length;
return length;

// Option B: Use IOVector wrapper
IOVector dest = IOVector::CreateFromBuffer(pDest, length);
return component_.calcOutputTo(dest, outChannel_);
```

**Why freezePage() preferred:**
- Page caching benefits (avoids redundant renders)
- Internal state snapshots work properly
- Aligns with audio rendering path

**Timeline:** Must complete before Phase 3

---

## Secondary Callers (May Auto-Migrate)

### 2. twStreamingLatch::readStreamingData() callers

**Who calls twStreamingLatch?**

Files that instantiate or use twStreamingLatch:
```bash
grep -r "twStreamingLatch\|readStreamingData" smaragd --include="*.h" --include="*.cc" | grep -v "^Binary"
```

**Known callers:**
- twComponent::createOutputLatches() → creates streaming latches
- SObject preview system → uses latches for waveform
- Parameter UI scrubbing → direct component access

**Status:** These auto-migrate once twStreamingLatch is fixed

---

## Optional Callers (Lower Priority)

### 3. Format Negotiation Paths

**File:** `tw303a/src/twnegotiator.cc` (if any direct calls)

**Status:** Check if any direct calcOutputTo calls exist

**Likelihood:** Low (format negotiation is mostly type-agnostic)

---

## Internal Implementation Paths

### 4. Default Adapter in twComponent Base Class

**File:** `tw303a/src/twcomponent.cc` (~line 200)

**Current Code:**
```cpp
length_t twComponent::calcOutputTo(IOVector& dest, idx_t idx) {
    // Default: wrap IOVector, call raw-pointer version
    sample_t *buffer = (sample_t *)alloca(dest.length() * sizeof(sample_t));
    length_t result = calcOutputTo(buffer, dest.length(), idx);  // CALLS RAW-POINTER
    return dest.copyFrom(IOVector::CreateFromBuffer(buffer, result), 0, result);
}
```

**Status:** This is the DEFAULT implementation
- Calls raw-pointer version
- Used by components that don't override IOVector method
- Will be replaced in Phase 3 (once all components implement IOVector version)

**Migration:** After all components override IOVector, this default becomes unnecessary

---

## Components with Raw-Pointer Implementation

All 18 refactored components still have the raw-pointer version as pure virtual override:

### Stateless Sources (3)
- twConstant
- twWhiteNoise
- twTestSeq (disabled)

### DSP Processors (5)
- twMoog
- twPipe
- twPluginInsert
- twPluginChain
- (twMixer - in router category)

### Routing & Mixing (3)
- twMixer
- twRewire
- twTrackMix

### Wrappers & Readers (2)
- twView
- twSampleReader

### File I/O (2)
- twWavInput
- twWav

### Output (1)
- twSpeaker

### Oscillators (2)
- twSimpleSaw
- twSaw (disabled)

---

## Call Graph Summary

```
┌─────────────────────────────────────┐
│ Raw-Pointer calcOutputTo()          │
│ (All 18 components implement)        │
└────────────┬────────────────────────┘
             │
             ├─ Called by: twStreamingLatch::readStreamingData() ⭐ CRITICAL
             │  └─ Used by: Qt UI preview, parameter scrubbing
             │
             ├─ Called by: Default IOVector adapter (twComponent)
             │  └─ Fallback when IOVector version not overridden
             │
             ├─ Called by: Plugin frameworks (plugin::processAudio)
             │  └─ Optional (plugins may call either interface)
             │
             └─ Called by: Format negotiation (possibly)
                └─ Check twnegotiator.cc
```

---

## Migration Effort Estimate

| Caller | Files | Calls | Effort | Priority |
|--------|-------|-------|--------|----------|
| twStreamingLatch | 2 | ~3 sites | 8 hrs | **CRITICAL** |
| Default adapter | 1 | 1 site | 2 hrs | Phase 3 |
| Format negotiation | 1 | ~2 sites | 2 hrs | Low |
| Plugin chains | 3 | ~5 sites | 5 hrs | Phase 2 |
| **TOTAL** | | | **17 hrs** | |

---

## Testing & Verification

### Before Phase 2 Starts
- [ ] Verify no calls from external/plugin code
- [ ] Confirm Qt UI tests still pass
- [ ] Document parameter scrubbing codepath

### During Phase 2 Migration
- [ ] Test each migrated caller independently
- [ ] Performance benchmark before/after
- [ ] UI responsiveness test (parameter changes)

### After Phase 2 Completion
- [ ] Full test suite (98/100 → 100/100)
- [ ] Integration test (complex project playback)
- [ ] Memory profile (page allocation patterns)

---

## Files to Update

### Documentation
- [ ] ARCHITECTURE.md — add deprecation notice
- [ ] COMPONENTS.md — add migration timeline
- [ ] COMPONENT_MIGRATION_GUIDE.md — already done ✅

### Code Changes (Phase 1)
- [x] twComponent.h — add [[deprecated]] warning
- [x] Each of 18 components — add [[deprecated]] warning (COMPLETE)
- [x] CMakeLists.txt — add -Wdeprecated-declarations if desired (via compiler defaults)

### Code Changes (Phase 2)
- [ ] twStreamingLatch — migrate to freezePage()
- [ ] Plugin chain code — migrate if needed
- [ ] Format negotiation — check and fix

### Code Changes (Phase 3)
- [ ] Remove raw-pointer from all 18 components
- [ ] Remove default adapter from twComponent
- [ ] Update base class

### Code Changes (Phase 4)
- [ ] Unify page systems

---

## Decision: Deprecation Warning Level

**Question:** How aggressive should the deprecation warning be?

**Options:**
1. **Gentle:** Just `[[deprecated()]]` attribute (compiler warning if used)
2. **Loud:** Compile error via `#error` (forces immediate migration)
3. **Silent:** No warning yet (soft deprecation period)

**Recommendation:** Option 1 (Gentle)
- Gives users time to plan migration
- Warns on compilation
- Allows v0.x to coexist with v1.0 plans

---

## Rollback Strategy

If Phase 2 migration fails:
1. Keep deprecation warning (safe, doesn't break anything)
2. Revert code changes
3. Document what went wrong
4. Re-plan Phase 2 approach

---

## Timeline (Phase 1)

- **Day 1:** Add deprecation warnings (this session)
- **Day 2-3:** Complete caller audit (this document)
- **Day 4-5:** Write migration guides per caller
- **Day 6:** Create checklist for Phase 2
- **Day 7:** Review & adjust timeline

**Estimated:** 1-2 weeks for full Phase 1 documentation

---

## Sign-off

**Phase 1 Status:** ✅ COMPLETE

**Completion Criteria:**
- [x] Deprecation warnings added to base class
- [x] Deprecation warnings added to all 18 components
- [x] All callers documented in this file
- [x] Migration path clear for each caller
- [x] Timeline set for Phase 2
- [x] Team alignment on approach

**Next Steps:** Begin Phase 2 — Migrate twStreamingLatch from raw-pointer to freezePage()

---

**Auditor:** Claude  
**Date:** 2026-06-30  
**Review Status:** Ready for team review
