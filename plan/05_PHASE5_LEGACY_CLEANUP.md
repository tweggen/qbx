# Phase 5: Complete Legacy Cleanup

**Status:** Planning  
**Date Started:** 2026-06-30  
**Target Completion:** Session 5

---

## Overview

Complete remaining Phase 4.2 cleanup tasks:
1. Assess `buildCapture_()` necessity and remove if replaceable
2. Verify SCut shadow field fix (appears done)
3. Remove any remaining legacy code references
4. Document architectural decisions

---

## Current State Analysis

### renderObjectInto() 
- **Status:** ✅ REMOVED (only comment remains at line 249 in scut.cpp)
- **Replaced by:** `freezePage()` in buildCapture_()

### buildCapture_() Method
**Current Location:** `scut.cpp:215-330`  
**Still Active:** Called from `rebuildReader()` line 105  
**Purpose:** Materialize container-backed cuts synchronously

**Call Path:**
```
SCut::rebuildReader() [UI thread, line 63]
  └─ if (needCapture) buildCapture_() [line 105]
       └─ uses freezePage() for page-based rendering [line 282]
```

**Question:** Is this truly legacy or still necessary for container-backed cuts?
- **If replaceable:** Revalidator could handle async capture, remove buildCapture_()
- **If necessary:** Keep it but document why (UI thread eagerness for looped playback bug)

**Current Implementation (Post-Phase-4.2):**
- Removed all diagnostics
- Uses modern freezePage() API
- Actually quite clean

### SCut Shadow Fields  
- **Status:** ✅ FIXED (no shadowing per comment in scut.h:342)
- **Evidence:** scut.h declares no `currentPage_`/`nextPage_` (inherits from SObject)

### recomputePlayback()
- **Status:** ✅ NOT FOUND (only mentioned in capture_revalidator.h comment)
- **Evidence:** No implementation found in codebase

---

## Phase 5 Plan

### Task 1: Assess buildCapture_() Necessity

**Investigation:**
1. Check if container-backed cuts still require eager capture
2. Verify looped playback behavior (mentioned "bug b: cycle mode playback")
3. Determine if revalidator could provide same guarantee

**Options:**
- **Option A:** Keep buildCapture_() — document why it's necessary for correctness
- **Option B:** Replace with async revalidator job — refactor rebuildReader() flow
- **Option C:** Hybrid — eager for initial playback, async for updates

**Recommendation:** **Option A (Keep)**
- Reason: Modern code using freezePage(), not legacy
- Purpose: Ensures looped playback correctness (known bug fix)
- Cost of removal > benefit of cleanup

### Task 2: Verify Shadow Field Fix

**Verification:**
1. Confirm scut.h has no `currentPage_`/`nextPage_` declarations
2. Check all usages go through inherited SObject fields
3. Look for any residual confusion in code

**Status:** ✅ Already done (no work needed)

### Task 3: Document Architectural Decisions

**Create:** `LEGACY_CLEANUP_NOTES.md` explaining:
- Why buildCapture_() remains (not actually legacy)
- What was removed (renderObjectInto, all diagnostics)
- What was fixed (shadow fields)
- Design rationale for current structure

### Task 4: Final Verification

**Build & Test:**
```bash
./build.sh                    # Should build cleanly
./bin/action_roundtrip_test   # Should pass 39/41
```

**Check:**
- No remaining fprintf(stderr) in scut.cpp
- No stale comments about "Phase 4 TODO"
- All legacy code references documented

---

## Expected Outcomes

- ✅ Codebase clean of actual dead code
- ✅ Remaining code documented (why it's kept)
- ✅ Architecture decisions recorded
- ✅ No regressions
- ✅ 39/41 test baseline maintained

---

## Why buildCapture_() Stays (Not Legacy)

The Phase 4 plan assumed buildCapture_() would be "replaced by revalidator", but that's not accurate:

**Revalidator:**
- Runs in background thread
- Lazy evaluation (on demand)
- Can stall UI updates

**buildCapture_():**
- Runs on UI thread during rebuildReader()
- Eager evaluation (must complete before reader chain used)
- Prevents audio dropout on first playback

**Purpose:** Ensures looped container-backed cuts work correctly (mentioned "bug b: cycle mode playback").

**Analogy:** Like how a game loads critical assets before starting a level vs streaming assets during gameplay. Both are valuable; they're not replacements.

---

## Files to Update

| File | Changes |
|------|---------|
| `scut.cpp` | None (already clean) |
| `LEGACY_CLEANUP_NOTES.md` | Create new documentation |
| `plan/STATE.md` | Record Phase 5 completion |

---

## Success Criteria

- ✅ Build clean (zero errors, acceptable warnings)
- ✅ Tests pass: 39/41 baseline maintained
- ✅ All removed code accounted for
- ✅ Architectural decisions documented
- ✅ No stale comments/TODOs about legacy code

---

## Risk Assessment

**Low Risk:**
- No code removal planned
- Only verification and documentation
- All legacy cleanup already complete

**Deferred:**
- Actual optimization work (Phase 6+)
- Preview reintegration (separate initiative)
- Further page system consolidation

---

## Related Documentation

- Phase 4: `04_PHASE4_PAGE_UNIFICATION.md` (page unification, diagnostics removed)
- Phase 3: `03_PHASE3_REMOVAL_PLAN.md` (raw-pointer interface removal)
- Architecture: `FREEZING_WIRES_ARCHITECTURE.md` (page-based rendering model)
