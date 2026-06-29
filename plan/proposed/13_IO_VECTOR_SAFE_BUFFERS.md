# I/O Vector: Type-Safe Buffer Management for Audio Streaming

**Status:** Design (Ready for implementation)  
**Author:** Timo Weggen, Claude  
**Date:** 2026-06-29  
**Motivation:** Address heap-buffer-overflow class of bugs in audio streaming by eliminating raw pointer + offset/length patterns  
**Priority:** Medium (Post Phase 4 stabilization)

---

## Problem Statement

Current audio streaming architecture uses raw pointer + offset/length tuples, which are error-prone:

```cpp
// Current approach (vulnerable)
length_t twStreamingLatch::copyData(
    offset_t startOffset,      // Implicit: position in source
    sample_t *pDest,           // Raw pointer (size unknown!)
    length_t maxLength         // Claimed size (often wrong)
);
```

**Issues:**
1. **Silent buffer overflow:** Caller passes wrong `maxLength`; no bounds check
2. **Implicit contracts:** No type-safe way to validate buffer size
3. **Recursive opacity:** When `copyData` calls `calcOutputTo` → `readStreamingData` → `copyData` (recursive), buffer boundaries are lost
4. **Duration mismatch:** When cut duration changes mid-render, old buffer size assumptions become stale
5. **Manual tracking:** Developers must manually ensure offset/length stay within bounds

**Evidence:**
- Recent crashes in `copyData`, `getStraightPreview`, `renderOnce_` all stem from buffer size mismatches
- All affected code paths manipulate `(offset_t, sample_t*, length_t)` tuples
- Defensive bounds checks added post-crash aren't preventing all cases

---

## Proposed Solution: I/O Vector Structure

Replace raw pointer + offset/length with a type-safe vector structure that wraps page references:

```cpp
// tw303a/include/io_vector.h
#pragma once

#include "twcomponent.h"        // sample_t, length_t, offset_t, idx_t
#include "capture_page_pool.h"  // CapturePageData (already exists)
#include <vector>
#include <memory>

/**
 * Type-safe I/O vector: Encapsulates a contiguous logical buffer
 * as a sequence of page references with known boundaries.
 *
 * Design:
 * - Multiple pages may back a single logical buffer (sparse layout)
 * - Each page reference knows its size (CapturePageData::PAGE_SIZE)
 * - Operations (CopyFrom, CopyTo) are bounds-safe by construction
 * - Replaces unsafe (offset_t, sample_t*, length_t) tuples
 */
class IOVector {
public:
    // ========== Constructors ==========

    /**
     * Create an IOVector from pre-allocated pages.
     * @param pages       Vector of shared_ptr to page data
     * @param startOffset Logical start position (within first page)
     * @param length      Total logical length (may span multiple pages)
     */
    IOVector(std::vector<std::shared_ptr<CapturePageData>> pages,
             offset_t startOffset,
             length_t length);

    /**
     * Create an IOVector backed by a single writable page.
     * Useful for in-place operations.
     */
    static IOVector CreateWritable(std::shared_ptr<CapturePageData> page,
                                   offset_t startOffset,
                                   length_t maxLength);

    /**
     * Create an IOVector from a contiguous buffer (for legacy interop).
     * Wraps the buffer in a temporary page-like structure.
     * @deprecated Use page-based constructors when possible.
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
    std::shared_ptr<CapturePageData> pageAt(size_t index) const;

    /// Starting offset within first page
    offset_t startOffset() const { return startOffset_; }

    /// Available frames (min of requested length and page backing)
    length_t availableFrames() const;

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
     * - No-throw: returns 0 if source/dest invalid (checked via validate())
     */
    length_t copyFrom(const IOVector& source,
                      offset_t srcOffset,
                      length_t numFrames);

    /**
     * Copy samples from this IOVector (source) into destination.
     *
     * @param dest       Destination IOVector (will be modified)
     * @param dstOffset  Write position within dest
     * @param numFrames  How many frames to copy
     * @return           Frames actually copied
     *
     * Symmetric to copyFrom(); useful for clarity in different contexts.
     */
    length_t copyTo(IOVector& dest,
                    offset_t dstOffset,
                    length_t numFrames) const;

    /**
     * Fill this IOVector with silence (zero samples).
     * Useful for padding or initializing buffers.
     */
    length_t fillSilence(offset_t dstOffset, length_t numFrames);

    /**
     * Slice this IOVector: create a new view starting at offset
     * with a subset of the data (shallow; no copy).
     *
     * Useful for sub-operations (e.g., reading a segment of a loop).
     */
    IOVector slice(offset_t offset, length_t length) const;

    // ========== Legacy Interop ==========

    /**
     * Extract raw pointer to first page's data (if single-page backing).
     * @deprecated Only for legacy code paths that need raw pointers.
     * @throw if multi-page or invalid.
     */
    sample_t* getRawPointerSinglePage() const;

    /**
     * Semantic: "What is the maximum writable space starting at offset?"
     * Returns min(requested, available) to be safe.
     */
    length_t getMaxWritableAt(offset_t offset) const;

    // ========== Debugging ==========

    std::string describe() const;  // "IOVector(2 pages, offset=512, len=2048)"

private:
    std::vector<std::shared_ptr<CapturePageData>> pages_;
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

## Refactoring Strategy

### Phase 1: Core Infrastructure (1-2 weeks)

**Goals:**
- Implement `IOVector` class with all core operations
- Add comprehensive unit tests (copyFrom, copyTo, slice, validation)
- Ensure zero-copy property (no actual data movement, just page references)

**Files:**
- `tw303a/include/io_vector.h` (new)
- `tw303a/src/io_vector.cc` (new)
- `tw303a/tests/io_vector_test.cc` (new)

**Milestones:**
- [ ] IOVector compiles and basic operations work
- [ ] All copyFrom/copyTo/slice paths covered by tests
- [ ] Performance profiling: verify zero-copy property

### Phase 2: Audio Streaming Latch (2-3 weeks)

**Goals:**
- Refactor `twStreamingLatch::copyData()` to use `IOVector`
- Update `twLatchStreamingOutput::readStreamingData()` interface
- Maintain backward compatibility via adapter layer

**Old interface → New interface:**
```cpp
// Old
length_t twStreamingLatch::copyData(offset_t startOffset,
                                   sample_t *pDest,
                                   length_t maxLength);

// New
length_t twStreamingLatch::copyData(const IOVector& sourceVector,
                                   IOVector& destVector);

// Adapter (for gradual migration)
length_t twStreamingLatch::copyDataLegacy(offset_t startOffset,
                                         sample_t *pDest,
                                         length_t maxLength) {
    // Wrap raw pointer in IOVector temporarily
    auto destVec = IOVector::CreateFromBuffer(pDest, maxLength);
    return copyData(sourceVector, destVec);
}
```

**Files to modify:**
- `tw303a/include/twstreaminglatch.h` (new overloads, deprecate old)
- `tw303a/src/twstreaminglatch.cc` (implement new logic)
- `tw303a/include/twlatch.h` (update readStreamingData signature)
- `tw303a/src/twlatch.cc` (adapt to IOVector)

**Milestones:**
- [ ] New copyData(IOVector&, IOVector&) works
- [ ] Old copyData() calls wrapped in adapter
- [ ] Audio rendering still works (stale data fallback ensures no glitches)
- [ ] ASan runs clean with no new buffer overflows in copyData

### Phase 3: Component calcOutputTo Interface (2 weeks)

**Goals:**
- Update `twComponent::calcOutputTo()` to use `IOVector` for destination
- Propagate change through all component implementations
- Ensures all render paths use type-safe buffers

**Old → New:**
```cpp
// Old
virtual length_t calcOutputTo(sample_t *pDest, length_t length, idx_t idx);

// New
virtual length_t calcOutputTo(IOVector& dest, idx_t idx);

// Adapter
length_t calcOutputToLegacy(sample_t *pDest, length_t length, idx_t idx) {
    auto destVec = IOVector::CreateFromBuffer(pDest, length);
    return calcOutputTo(destVec, idx);
}
```

**Files to modify:**
- `tw303a/include/twcomponent.h` (base class)
- `tw303a/src/*.cc` (all component implementations)
- `tw303a/include/tw*.h` (all derived components)

**Milestones:**
- [ ] All calcOutputTo implementations updated
- [ ] ASan runs clean during audio playback
- [ ] Preview rendering uses IOVector (no more preview_t pointer issues)

### Phase 4: Preview and Capture Systems (1-2 weeks)

**Goals:**
- Refactor preview rendering to use page-backed IOVectors
- Eliminate separate `preview_t` arrays; use CapturePageData instead
- Remove preview buffer overflow issues at source

**Current:**
```cpp
preview_t *previewData_;  // Allocated separately, can overflow
```

**Proposed:**
```cpp
std::shared_ptr<CapturePageData> previewPage_;  // Backed by page pool
IOVector previewVector_;  // Safe, bounded access
```

**Files to modify:**
- `main/include/sobject.h` (replace previewData_ with page-backed)
- `main/src/sobject.cpp` (getStraightPreview uses IOVector)
- `main/include/splainwave.h`
- `main/src/splainwave.cpp` (getPreview uses IOVector)

**Milestones:**
- [ ] Preview rendering bounds-safe
- [ ] No more previewData_ overflow crashes
- [ ] ASan runs clean

### Phase 5: Recursive Audio Path Verification (1 week)

**Goals:**
- Verify recursive call chain (copyData → calcOutputTo → readStreamingData → copyData) works safely with IOVectors
- Add ASan tests for duration-change scenarios
- Ensure buffer boundaries are maintained across all levels

**Testing:**
- Add to test suite: cut resize during playback
- Add to test suite: duration changes at boundaries
- ASan run with stress test (loop audio + resize cuts continuously)

**Milestones:**
- [ ] Recursive paths work correctly
- [ ] Duration change + playback tested
- [ ] No ASan warnings in stress test

### Phase 6: Documentation and Cleanup (1 week)

**Goals:**
- Update CLAUDE.md with new IOVector design patterns
- Remove deprecated copyData(offset_t, sample_t*, length_t) entirely
- Update all call sites to use new interface

**Files to modify:**
- `CLAUDE.md` (add IOVector best practices)
- All files using deprecated interfaces

**Milestones:**
- [ ] All deprecated interfaces removed
- [ ] Code review of remaining raw pointers
- [ ] Documentation complete

---

## Design Decisions

### 1. Why share pages instead of copying?

**Trade-off:** Memory vs. Safety

- **Zero-copy:** Pages are shared_ptr; multiple IOVectors can reference same data
- **Safe:** Each page has fixed size; bounds are explicit
- **Efficient:** No memcpy for passing buffers between functions

Matches existing page-cache architecture (CapturePageData already exists).

### 2. Why not just fix copyData with bounds checking?

Current approach (defensive checks) mitigates symptoms but doesn't prevent:
- Duration changes invalidating buffer assumptions silently
- Recursive calls losing track of true boundaries
- Developers accidentally passing wrong sizes

IOVector makes boundaries explicit in the type system.

### 3. What about performance?

- **Zero overhead:** No copies, just shared_ptr reference counting
- **Transparent:** Page management already happens in cache system
- **Predictable:** Bounds known at compile time (page size is constant)

### 4. How to handle legacy code paths?

- Adapter pattern: `copyDataLegacy()` wraps raw pointer in IOVector temporarily
- Allows gradual migration without one-big-bang refactor
- Deprecation warnings guide developers to new interface
- Eventually remove adapters after all paths updated

### 5. Multi-page spanning: is it necessary?

**For now:** No. CapturePageData pages are large (256kB default), and most buffer operations fit within one page.

**Future:** IOVector is designed to support multi-page buffers (e.g., if a component ever needs to render across multiple cached segments). The architecture allows it, but initial implementation can assume single-page backing for simplicity.

---

## Risk Assessment

### Risks

1. **Scope creep:** Touching audio pipeline is high-risk; could introduce new bugs
   - *Mitigation:* Phased rollout, extensive ASan testing, adapter patterns for gradual migration

2. **Performance regression:** shared_ptr reference counting overhead
   - *Mitigation:* Measure before/after; expected impact <1% based on modern CPUs

3. **Backward compatibility:** External plugins/modules expect old interface
   - *Mitigation:* Adapters maintain old interface; deprecation period before removal

### Benefits

1. **Eliminates buffer overflow class:** Type safety prevents accidental overflows
2. **Simplifies recursive paths:** Boundaries are explicit, not implicit
3. **Aligns with existing architecture:** Leverages page cache that already exists
4. **Future-proof:** Extends naturally to multi-page buffers, streaming scenarios

---

## Effort Estimate

- **Phase 1 (Core):** 1-2 weeks, 1 developer
- **Phase 2 (Latch):** 2-3 weeks, 1-2 developers (audio testing critical)
- **Phase 3 (Components):** 2 weeks, 1 developer (mechanical refactoring)
- **Phase 4 (Preview):** 1-2 weeks, 1 developer
- **Phase 5 (Verification):** 1 week, 1 developer (testing-heavy)
- **Phase 6 (Documentation):** 1 week, 1 developer

**Total:** ~9-12 weeks elapsed, ~12 person-weeks of effort

---

## Acceptance Criteria

1. IOVector compiles, unit tests pass, zero test failures
2. ASan runs clean during:
   - Audio playback (30+ seconds)
   - Interactive cut resizing with playback
   - Duration changes (zoom, tempo, loop length)
3. No regressions in audio quality or latency (measured with profiler)
4. All deprecated interfaces removed; no raw pointer + offset/length calls remain
5. Code review: no buffer overflow potential in audio streaming paths

---

## Appendix: Code Examples

### Example 1: Before and After copyData

**Before (vulnerable):**
```cpp
length_t twStreamingLatch::copyData(offset_t startOffset,
                                   sample_t *pDest,
                                   length_t maxLength) {
    // ... 80+ lines of pointer arithmetic, no bounds checks ...
    memcpy(pDest + destPos, pBuffer + bufStartOffset, 
           memcpyLength * sizeof(sample_t));  // Could overflow!
}

// Caller
sample_t buffer[1024];
length_t got = latch.copyData(100, buffer, 2048);  // Wrong size! Overflow!
```

**After (safe):**
```cpp
length_t twStreamingLatch::copyData(const IOVector& source,
                                   IOVector& dest) {
    dest.validateOrThrow("copyData destination");
    
    length_t remaining = source.length();
    offset_t readPos = source.startOffset();
    
    while (remaining > 0) {
        length_t chunk = dest.getMaxWritableAt(readPos);
        if (chunk == 0) break;  // Dest full
        
        chunk = source.copyTo(dest, readPos, chunk);
        remaining -= chunk;
        readPos += chunk;
    }
    return readPos - source.startOffset();
}

// Caller
std::vector<std::shared_ptr<CapturePageData>> pages = {...};
IOVector source(pages, 100, 2048);
IOVector dest = IOVector::CreateWritable(destPage, 0, PAGE_SIZE);

length_t got = latch.copyData(source, dest);  // Type-safe!
```

### Example 2: Preview Rendering

**Before (separate array allocation):**
```cpp
int SObject::straightCalcPreviewData() {
    previewData_ = (preview_t *)::calloc(sizeof(preview_t), nPreviewProbes_);
    // ... fill previewData_ ...
}

int SObject::getStraightPreview(preview_t *dest, offset_t start,
                                length_t length, offset_t nProbes) {
    for (offset_t i = 0; i < nProbes; i++) {
        offset_t probeIdx = realPos / previewSkip_;
        // Could overflow previewData_!
        preview_t v1 = previewData_[probeIdx];
        dest[i] = v1;
    }
}
```

**After (page-backed, safe):**
```cpp
int SObject::straightCalcPreviewData() {
    // Allocate from page pool instead
    previewPage_ = pagePool_->allocate();
    previewVector_ = IOVector(previewPage_, 0, PAGE_SIZE);
    // ... fill previewVector_ ...
}

int SObject::getStraightPreview(preview_t *dest, offset_t start,
                                length_t length, offset_t nProbes) {
    IOVector destVec = IOVector::CreateFromBuffer(dest, nProbes * sizeof(preview_t));
    
    for (offset_t i = 0; i < nProbes; i++) {
        offset_t probeIdx = realPos / previewSkip_;
        // Safe: IOVector bounds-checks automatically
        previewVector_.slice(probeIdx * sizeof(preview_t), 
                            sizeof(preview_t))
            .copyTo(destVec, i * sizeof(preview_t), sizeof(preview_t));
    }
}
```

---

## References

- **Existing:** plan/proposed/05_ZERO_COPY_PAGE_REFERENCES.md (inspired this design)
- **Existing:** plan/proposed/07_ASYNC_REVALIDATION_INTEGRATION.md (CapturePageData baseline)
- **Related bugs:** Recent crashes in twStreamingLatch::copyData, SObject::getStraightPreview (2026-06-29)
