# I/O Vector: Type-Safe Buffer Management for Audio Streaming (V2)

**Status:** Design (Ready for implementation)  
**Author:** Timo Weggen, Claude  
**Date:** 2026-06-30  
**Motivation:** Address heap-buffer-overflow class of bugs in audio streaming by eliminating raw pointer + offset/length patterns. Updated for V3 unified rendering architecture.  
**Priority:** Medium (Post Phase 5 stabilization)

---

## Context: V3 Unified Rendering Architecture

The recent refactoring (Phases 1-5) introduced:
- **Page-based freezing** with explicit state snapshots (`freezePage()` API)
- **twView wrapper pattern** for stable clip component references
- **Callback-based clip management** (`std::function<twComponent*()>`)
- **Strict decoupling** between tw303a (DSP) and main/ (UI)

This document adapts the IOVector proposal to work **synergistically** with these changes, rather than replacing them. IOVector becomes the **buffer abstraction layer** within pages and streaming latches.

---

## Problem Statement (Updated)

While the V3 architecture solves **stale pointer crashes** and **page boundary state issues**, it doesn't address the **buffer overflow** vulnerability class in:

1. **Streaming latches** (`twStreamingLatch::copyData`)
   - Old: `copyData(offset_t startOffset, sample_t *pDest, length_t maxLength)`
   - Issue: Caller can pass wrong `maxLength`; no type-safe bounds

2. **Format conversion** and **mixing** within pages
   - Pages contain raw sample arrays; mixing is manual pointer arithmetic
   - When multiple clips mix into a page, off-by-one errors in offset calculations cause silent corruption

3. **Preview rendering** (SObject → preview_t arrays)
   - Separate buffer allocation; not integrated with page pool
   - Buffer resizes during playback can invalidate cached pointers

4. **Legacy calcOutputTo still exists** for some components
   - Still uses `(sample_t *pDest, length_t length)` pattern
   - Mixing freezePage and calcOutputTo code paths is error-prone

**Evidence:**
- Recent crashes in `twStreamingLatch::copyData`, `getStraightPreview`, `renderOnce_` all stem from buffer size mismatches
- Page-based rendering solves *timing* issues but doesn't enforce *buffer safety*
- Mix operations within `freezePage_nolock` still manipulate offsets manually

---

## Proposed Solution: I/O Vector Structure (Updated)

Keep the core concept from the original proposal, but integrate it with **page-backed buffers** and **freezePage() semantics**:

```cpp
// tw303a/include/io_vector.h
#pragma once

#include "twcomponent.h"        // sample_t, length_t, offset_t, idx_t
#include "tw_output_page.h"     // twOutputPage (page-based rendering)
#include <vector>
#include <memory>

/**
 * Type-safe I/O vector: Encapsulates a contiguous logical buffer
 * as a sequence of page references with known boundaries.
 *
 * V3 Integration:
 * - Backed by twOutputPage (unified page model) or CapturePageData
 * - Operations (copyFrom, copyTo, mix) are bounds-safe by construction
 * - Replaces unsafe (offset_t, sample_t*, length_t) tuples
 * - Works with freezePage callback chain and twView wrappers
 *
 * Use Cases:
 * 1. Streaming latch copyData: Safe buffer -> buffer transfer
 * 2. Mixing clips in twTrackMix::freezePage_nolock: Type-safe offset arithmetic
 * 3. Preview rendering: Page-backed instead of separate arrays
 * 4. Format conversion: Known bounds prevent int overflow
 */
class IOVector {
public:
    // ========== Constructors ==========

    /**
     * Create an IOVector from a twOutputPage (page-based rendering).
     * Most common case: wraps page samples array with known size.
     *
     * @param page        Shared reference to output page
     * @param startOffset Offset within page's sample buffer (0 = beginning)
     * @param length      Logical length (frames); clamped to available
     */
    IOVector(std::shared_ptr<twOutputPage> page,
             offset_t startOffset,
             length_t length);

    /**
     * Create an IOVector from multiple pages (rare; for future multi-page buffers).
     *
     * @param pages       Vector of shared_ptr to pages
     * @param startOffset Logical start position (within first page)
     * @param length      Total logical length (may span multiple pages)
     */
    IOVector(std::vector<std::shared_ptr<twOutputPage>> pages,
             offset_t startOffset,
             length_t length);

    /**
     * Create a writable IOVector for destination rendering.
     * Typical use: output of a clip's freezePage call.
     */
    static IOVector CreateForPageOutput(std::shared_ptr<twOutputPage> page);

    /**
     * Create an IOVector from a contiguous buffer (legacy interop).
     * Wraps the buffer in a temporary structure; NOT memory-safe
     * across page boundaries. Only for legacy paths and tests.
     *
     * @deprecated Use page-backed constructors when possible.
     */
    static IOVector CreateFromBuffer(sample_t* buffer,
                                     length_t lengthFrames);

    // ========== Validation ==========

    /**
     * Validate structure: check all page references are valid,
     * offsets are within page bounds, length doesn't exceed available.
     */
    bool validate() const;

    /**
     * Throw if invalid. Used at API boundaries to catch errors early.
     */
    void validateOrThrow(const char* context) const;

    // ========== Accessors ==========

    /// Logical length (frames)
    length_t length() const { return length_; }

    /// Number of pages backing this vector
    size_t pageCount() const { return pages_.size(); }

    /// Get the i-th page (range-checked)
    std::shared_ptr<twOutputPage> pageAt(size_t index) const;

    /// Starting offset within first page
    offset_t startOffset() const { return startOffset_; }

    /// Available frames (min of requested length and page backing)
    length_t availableFrames() const;

    /// Raw pointer to first page's sample buffer (if single-page; else throw)
    sample_t* rawPointer() const;

    // ========== Operations ==========

    /**
     * Copy samples from source IOVector into this IOVector (destination).
     *
     * @param source     Source IOVector (const, safe to share)
     * @param srcOffset  Read position within source
     * @param numFrames  How many frames to copy
     * @return           Frames actually copied (may be less if bounds hit)
     *
     * Properties:
     * - Bounds-safe: won't read/write past page boundaries
     * - Handles wraparound: if source spans multiple pages, handles correctly
     * - Partial copy OK: if only N < numFrames are available, copies N
     * - No-throw: returns 0 if source/dest invalid
     */
    length_t copyFrom(const IOVector& source,
                      offset_t srcOffset,
                      length_t numFrames);

    /**
     * Copy samples from this IOVector (source) into destination.
     * Symmetric to copyFrom(); useful for clarity.
     */
    length_t copyTo(IOVector& dest,
                    offset_t dstOffset,
                    length_t numFrames) const;

    /**
     * Mix (add) samples from source into this IOVector at given offset.
     * Used for clip mixing in twTrackMix::freezePage_nolock.
     *
     * @param source     Source IOVector to mix from
     * @param dstOffset  Destination offset (where to add into)
     * @param numFrames  How many frames to mix
     * @return           Frames actually mixed
     *
     * Semantics: dest[dstOffset + i] += source[i] for i in [0, numFrames)
     */
    length_t mixFrom(const IOVector& source,
                     offset_t dstOffset,
                     length_t numFrames);

    /**
     * Fill this IOVector with silence (zero samples).
     * Useful for initialization and padding.
     */
    length_t fillSilence(offset_t dstOffset, length_t numFrames);

    /**
     * Slice this IOVector: create a new view starting at offset
     * with a subset of the data (shallow; no copy).
     *
     * Useful for sub-operations (e.g., mixing a segment of a clip).
     */
    IOVector slice(offset_t offset, length_t length) const;

    // ========== Debugging ==========

    std::string describe() const;  // "IOVector(page@1000, off=0, len=65536)"

private:
    std::vector<std::shared_ptr<twOutputPage>> pages_;
    offset_t startOffset_;
    length_t length_;

    // Internal helper: map logical offset to (page_index, offset_in_page)
    struct LogicalToPhysical {
        size_t pageIndex;
        offset_t offsetInPage;
    };
    LogicalToPhysical mapOffset(offset_t logical) const;
};
```

---

## V3 Integration Points

### 1. twTrackMix::freezePage_nolock (Mixing with Type Safety)

**Before (manual offset arithmetic, error-prone):**
```cpp
// tw303a/src/twtrackmix.cc (current)
for( ClipEntry &clip : clips_ ) {
    auto childPage = clip.view->freezePage(childPos, ..., clip.previousPage);
    
    // Manual offset calculation: prone to off-by-one errors
    uint64_t destOffset = (clip.startTime >= startPos)
                          ? (clip.startTime - startPos)
                          : 0;
    
    for( uint32_t i = 0; i < childPage->validFrames && destOffset + i < length; ++i ) {
        if( i < childPage->samples.size() ) {
            page->samples[destOffset + i] += childPage->samples[i];  // Manual mix
        }
    }
}
```

**After (type-safe mixing):**
```cpp
for( ClipEntry &clip : clips_ ) {
    auto childPage = clip.view->freezePage(childPos, ..., clip.previousPage);
    
    // Create IOVectors for both source and destination
    IOVector sourceVec = IOVector::CreateForPageOutput(childPage);
    IOVector destVec(page, 0, length);  // Output page
    
    // Mix offset calculation now type-checked
    offset_t destOffset = (clip.startTime >= startPos)
                          ? (clip.startTime - startPos)
                          : 0;
    
    // Type-safe mix operation
    destVec.mixFrom(sourceVec, destOffset, sourceVec.length());
}
```

**Benefits:**
- `mixFrom()` validates bounds automatically
- No manual array indexing with potential overflow
- Offset arithmetic is encapsulated and tested

### 2. Streaming Latch copyData (Safe Buffer Transfer)

**Before (vulnerable):**
```cpp
length_t twStreamingLatch::copyData(offset_t startOffset,
                                   sample_t *pDest,
                                   length_t maxLength) {
    // ... complex logic to copy from internal buffer to destination ...
    memcpy(pDest, pBuffer + offset, size);  // Could overflow!
}
```

**After (type-safe):**
```cpp
length_t twStreamingLatch::copyData(IOVector& destVector) {
    destVector.validateOrThrow("twStreamingLatch::copyData destination");
    
    // Internal buffer wrapped in IOVector
    IOVector sourceVec(internalBuffer_, 0, internalBufferSize_);
    
    return destVector.copyFrom(sourceVec, 0, sourceVec.length());
}

// Caller (from calcOutputTo or freezePage)
IOVector outputVec(outputPage, offset, length);
latch.copyData(outputVec);  // Type-safe
```

### 3. twView Callback Chain Compatibility

The twView wrapper pattern is **orthogonal** to IOVector:
- twView handles **component identity** (stable references)
- IOVector handles **buffer safety** (bounds checking)

They compose naturally:
```cpp
// STrack registers clip with twTrackMix
auto getComponentFn = [&child]() { return &child.getRootComponent(); };
trackMix->insertClip(startTime, duration, getComponentFn);

// Audio thread: twView forwards to current component
twComponent *comp = getComponentFn();  // Dynamic lookup via callback
auto page = comp->freezePage(...);     // Returns twOutputPage

// Mix into track output using IOVector
IOVector clipVec = IOVector::CreateForPageOutput(page);
trackOutputVec.mixFrom(clipVec, destOffset, clipVec.length());
```

---

## Refactoring Strategy (V3-Aware)

### Phase 1: Core IOVector Infrastructure (1-2 weeks)

**Goals:**
- Implement IOVector with twOutputPage backing
- Unit tests for copyFrom, copyTo, mixFrom, slice operations
- Verify zero-copy property (only shared_ptr references, no data movement)

**Files:**
- `tw303a/include/io_vector.h` (new)
- `tw303a/src/io_vector.cc` (new)
- `tw303a/tests/io_vector_test.cc` (new)

**Changes from original proposal:**
- Single-page focus (twOutputPage is large: 256 kB = 65,536 frames @ 48 kHz)
- Multi-page support in API but not required for MVP
- Integrates with page cache that already exists in V3

**Milestones:**
- [ ] IOVector compiles with twOutputPage backend
- [ ] copyFrom, copyTo, mixFrom all tested
- [ ] Performance verified: zero-copy property holds

### Phase 2: twTrackMix Mixing (1 week)

**Goals:**
- Replace manual offset arithmetic in `freezePage_nolock` with IOVector operations
- Ensure clip mixing is type-safe and bounds-checked

**Files to modify:**
- `tw303a/src/twtrackmix.cc` (freezePage_nolock method)

**Changes:**
- Wrap output page and child pages in IOVector
- Use `mixFrom()` instead of manual loop
- Remove manual boundary checks (IOVector does it)

**Milestones:**
- [ ] Audio playback still works (mixing correct)
- [ ] No ASan warnings in mixing code
- [ ] Mixed output audibly identical to before

### Phase 3: Streaming Latch (1-2 weeks)

**Goals:**
- Refactor `twStreamingLatch::copyData` to use IOVector
- Maintain backward compatibility via adapter layer

**Files to modify:**
- `tw303a/include/twstreaminglatch.h` (new overloads)
- `tw303a/src/twstreaminglatch.cc` (new implementation)

**Old → New:**
```cpp
// Old (vulnerable)
length_t copyData(offset_t startOffset, sample_t *pDest, length_t maxLength);

// New (safe)
length_t copyData(IOVector& destVector);

// Adapter (for gradual migration)
length_t copyDataLegacy(offset_t startOffset, sample_t *pDest, length_t maxLength) {
    auto destVec = IOVector::CreateFromBuffer(pDest, maxLength);
    return copyData(destVec);
}
```

**Milestones:**
- [ ] New copyData works with IOVector
- [ ] Audio rendering still works
- [ ] No buffer overflows in ASan

### Phase 4: Preview System (1-2 weeks)

**Goals:**
- Refactor preview rendering to use page-backed IOVectors
- Eliminate separate `preview_t` array allocations

**Files to modify:**
- `main/include/sobject.h` (replace previewData_ field)
- `main/src/sobject.cpp` (getStraightPreview, straightCalcPreviewData)
- `main/include/splainwave.h`, `main/src/splainwave.cpp`

**Changes:**
- Use page pool for preview buffers instead of manual malloc
- Wrap preview pages in IOVector for bounds-safe access
- Update getPreview() to use IOVector operations

**Milestones:**
- [ ] Preview rendering works correctly
- [ ] No buffer overflows in preview code
- [ ] ASan clean during preview updates

### Phase 5: Legacy Path Cleanup (1 week)

**Goals:**
- Remove deprecated `calcOutputTo(sample_t*, length_t)` from all components that now use freezePage
- Keep adapter for truly legacy paths only

**Files to modify:**
- All component implementations that still have old calcOutputTo

**Milestones:**
- [ ] All audio paths use page-based rendering (freezePage or IOVector-wrapped calcOutputTo)
- [ ] No raw pointer arithmetic in hot paths
- [ ] Code review: zero buffer overflow potential

### Phase 6: Documentation (1 week)

**Goals:**
- Update CLAUDE.md with IOVector best practices
- Document integration with twView and freezePage
- Remove deprecated patterns

**Milestones:**
- [ ] Documentation complete
- [ ] Code examples show IOVector usage
- [ ] All raw pointer + offset/length patterns flagged as deprecated

---

## Design Decisions (V3-Adjusted)

### 1. Why page-backed instead of custom memory pool?

V3 already uses pages (twOutputPage) for all rendering. IOVector leverages this:
- **Consistency:** Same abstraction as the rendering engine
- **Simplicity:** No additional memory management
- **Compatibility:** Works with existing page cache and freezePage API

### 2. Why add mixFrom() operation?

The mixing operation (add, not copy) is hot-path critical in `freezePage_nolock`. Providing a type-safe primitive:
- Eliminates manual loop in mixing code
- Ensures offset arithmetic is validated
- Enables future SIMD optimization (vectorized mixing)

### 3. What about multi-page spanning?

IOVector API supports it, but:
- **MVP:** Assume single-page (pages are 256 KB, covering 1.4 seconds @ 48 kHz)
- **Future:** If streaming very large buffers, multi-page support already designed

### 4. Why not extend freezePage API directly?

freezePage is the **right level** but adding IOVector is about **buffer safety** within pages:
- freezePage signature stays stable (input/output contracts don't change)
- IOVector is an **implementation detail** of mixing operations
- Decouples page management from buffer manipulation

---

## Risk Assessment (V3-Aware)

### Risks

1. **Scope:** Touching hot mixing path in freezePage_nolock
   - **Mitigation:** Phase 2 starts with small change, extensive testing
   - **Advantage:** V3 architecture already isolates this code (freezePage_nolock is one place)

2. **Performance:** shared_ptr reference counting in hot loop
   - **Mitigation:** Measure before/after; expected impact <0.5% on modern CPUs
   - **Note:** V3 already uses shared_ptr for pages; IOVector is minimal overhead

3. **Backward compatibility:** Legacy calcOutputTo still needs to work
   - **Mitigation:** Adapter pattern; deprecation period for removal
   - **Advantage:** V3 decoupling already reduced calcOutputTo usage

### Benefits

1. **Type-safe mixing:** Eliminates entire class of off-by-one errors
2. **Page integration:** Synergizes with V3 architecture (no fighting the design)
3. **Future-proof:** Supports streaming, format conversion, plugin state snapshots
4. **Testable:** Each IOVector operation is independently testable

---

## Effort Estimate (V3-Adjusted)

- **Phase 1 (Core IOVector):** 1-2 weeks, 1 developer
- **Phase 2 (Mixing):** 1 week, 1 developer (small, focused)
- **Phase 3 (Streaming latch):** 1-2 weeks, 1-2 developers
- **Phase 4 (Preview):** 1-2 weeks, 1 developer
- **Phase 5 (Cleanup):** 1 week, 1 developer
- **Phase 6 (Documentation):** 1 week, 1 developer

**Total:** ~6-9 weeks elapsed, ~8-10 person-weeks (down from 12 due to V3 simplifications)

---

## Acceptance Criteria

1. IOVector compiles, all unit tests pass, zero failures
2. ASan runs clean during:
   - Audio playback with multiple clips (30+ seconds)
   - Interactive clip mixing with gain/mute changes
   - Preview rendering during playback
3. No regressions in audio quality, latency, or mixing output
4. All deprecated raw pointer + offset/length interfaces removed
5. Code review: no buffer overflow potential in mixing or streaming paths
6. Documentation updated with IOVector best practices

---

## Summary: V3 + IOVector Composition

| Concern | Solved By |
|---------|-----------|
| Stale component pointers | **twView wrapper** (V3) |
| Page boundary state loss | **freezePage with snapshots** (V3) |
| Decoupling tw303a ↔ main/ | **Callback-based clip management** (V3) |
| Buffer overflows in mixing | **IOVector** (this proposal) |
| Buffer overflows in latches | **IOVector** (this proposal) |
| Preview memory safety | **IOVector + page pool** (this proposal) |

Together, they address **all three tiers** of safety:
1. **Component safety:** stable wrappers (twView)
2. **State safety:** explicit snapshots (freezePage)
3. **Buffer safety:** bounds-checked operations (IOVector)

---

## References

- **V3 Architecture:** docs/UNIFIED_RENDERING_ARCHITECTURE_V3.md
- **Original proposal:** plan/proposed/13_IO_VECTOR_SAFE_BUFFERS.md
- **Related:** plan/proposed/05_ZERO_COPY_PAGE_REFERENCES.md (inspired IOVector design)
- **Related:** plan/proposed/07_ASYNC_REVALIDATION_INTEGRATION.md (CapturePageData baseline)
- **Recent crashes:** Buffer overflows in twStreamingLatch::copyData, getStraightPreview (2026-06-29)
