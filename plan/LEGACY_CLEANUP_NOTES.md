# Legacy Cleanup: Final Assessment

**Date:** 2026-06-30  
**Status:** Phase 5 completion  
**Baseline:** After Phase 4 (page unification + diagnostics removed)

---

## What Was Removed

### ✅ renderObjectInto() — REMOVED
**Location:** Was `scut.cpp:219-373` in Phase 4 plan  
**Status:** Only a comment remains at line 249  
**Reason:** Recursive offline renderer for container/grained cuts  
**Replaced by:** `freezePage()` API (modern, page-based, called from buildCapture_())  
**Impact:** No functional loss; modern approach is cleaner

**Comment left behind:**
```cpp
// Instead of recursive offline rendering (renderObjectInto), call freezePage
```

### ✅ All Diagnostics — REMOVED (Phase 4.2)
**From:** `buildCapture_()`, `rebuildReader()`, `invalidateCapture()`, `seekTo()`, etc.  
**Total removed:** 48 lines of fprintf(stderr) calls  
**Impact:** Cleaner development console, no functional changes

### ✅ Commented-out ensureCapture() — REMOVED (Phase 4.2)
**Was:** Old API with Phase 3 replacement note  
**Removed:** 11 lines of dead code  
**Impact:** API surface cleaner

---

## What Was Fixed

### ✅ SCut Shadow Fields — FIXED (Earlier phase)
**Issue:** Both SObject and SCut had `currentPage_` / `nextPage_` declarations  
**Status:** Fixed (no shadowing; SCut inherits from SObject)  
**Evidence:** scut.h line 342 comment confirms inheritance, no local declaration  
**Impact:** No reference ambiguity, single canonical fields

### ✅ Page Interface Unification (Phase 4.1)
**Result:** `PageBase` abstract class for both twOutputPage and CapturePageData  
**Impact:** Enables polymorphic rendering code  
**Note:** Not a removal, but consolidation

---

## What Remains (Intentionally)

### buildCapture_() — KEPT (Not Actually Legacy)
**Location:** `scut.cpp:215-330`  
**Status:** Active, called from `rebuildReader()` line 105  
**Current implementation:**
- Uses modern `freezePage()` API (page-based rendering)
- Materializes container-backed cuts to reusable capture
- Runs synchronously on UI thread (not async)

**Why it remains:**
1. **Not legacy:** Uses modern freezePage() API, not old rendering path
2. **Necessary:** Ensures looped playback correctness for container cuts
3. **Design:** Eager materialization on UI thread prevents audio dropout on first play

**Analogy:** Like critical asset loading before game level starts—not redundant with background streaming.

**Could be removed if:**
- Revalidator could guarantee async capture before first audio callback (unlikely)
- Looped container-backed cuts no longer needed (breaking change)

**Phase 4 plan assumption:** "Synchronous capture builder that was replaced by revalidator"  
**Reality:** Different design patterns (eager vs. lazy), not replacements

---

## What Was Never Found

### ❌ recomputePlayback() — NOT FOUND
**Mentioned in:** capture_revalidator.h comment (outdated documentation)  
**Actual status:** No implementation in codebase  
**Impact:** Nothing to remove

---

## Verification Results

### Build Status
- ✅ Clean compilation
- ✅ Zero errors
- ✅ Acceptable warnings (pre-existing)

### Test Status  
- ✅ 39/41 tests passing (baseline maintained)
- ✅ No new failures
- ✅ Zero regressions

### Code Quality
- ✅ No fprintf(stderr) diagnostics in scut.cpp
- ✅ No stale "Phase X TODO" comments
- ✅ Architecture decisions documented

---

## Summary: Cleanup Complete

**Phase 3:** Raw-pointer interface removal (18 components)  
**Phase 4:** Page system unification + diagnostics cleanup  
**Phase 5:** Legacy code assessment and documentation

**Outcome:**
- All actual dead code removed (renderObjectInto, diagnostics)
- Remaining code is modern, documented, and necessary
- Architecture clean and stable
- Ready for optimization or new features

**What's left is not legacy—it's current.**

---

## For Future Maintainers

If you wonder why buildCapture_() still exists:
- It's not a remnant of old design
- It's necessary for container-backed cut correctness
- Modern page-based implementation
- Can only be removed if looped containers fundamentally change

The revalidator and buildCapture_() coexist by design:
- **Revalidator:** Background, lazy, updates when needed
- **buildCapture_():** Foreground, eager, synchronization point

Both serve real purposes. Neither is a replacement for the other.

---

## References

- Phase 4 plan: `04_PHASE4_PAGE_UNIFICATION.md`
- Phase 3 plan: `03_PHASE3_REMOVAL_PLAN.md`
- Architecture: `FREEZING_WIRES_ARCHITECTURE.md`
- State history: `plan/STATE.md`
