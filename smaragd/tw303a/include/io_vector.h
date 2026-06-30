#ifndef _IO_VECTOR_H
#define _IO_VECTOR_H

#include "twcomponent.h"  // sample_t, length_t, offset_t, idx_t
#include "tw_output_page.h"
#include <vector>
#include <memory>
#include <string>

/**
 * Type-safe I/O vector: Encapsulates a contiguous logical buffer
 * as a sequence of page references with known boundaries.
 *
 * V3 Integration:
 * - Backed by twOutputPage (unified page model) or multiple pages
 * - Operations (copyFrom, copyTo, mixFrom) are bounds-safe by construction
 * - Replaces unsafe (offset_t, sample_t*, length_t) tuples
 * - Works with freezePage callback chain and twView wrappers
 *
 * Design:
 * - Zero-copy: pages are shared_ptr; multiple IOVectors can reference same data
 * - Bounds-safe: each page has fixed size; operations validate against boundaries
 * - Composable: slice() creates views; no data copying
 *
 * Use Cases:
 * 1. Streaming latch copyData: Safe buffer -> buffer transfer
 * 2. Mixing clips in twTrackMix::freezePage_nolock: Type-safe offset arithmetic
 * 3. Preview rendering: Page-backed instead of separate arrays
 * 4. Format conversion: Known bounds prevent overflow
 */
class IOVector {
public:
    // ========== Constructors ==========

    /**
     * Create an IOVector from a single twOutputPage (common case).
     *
     * @param page        Shared reference to output page
     * @param startOffset Offset within page's sample buffer (0 = beginning)
     * @param length      Logical length (frames); clamped to available
     */
    IOVector(std::shared_ptr<twOutputPage> page,
             offset_t startOffset,
             length_t length);

    /**
     * Create an IOVector from multiple pages (for future multi-page buffers).
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
     * Starts at offset 0, uses full page size.
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
     *
     * @return true if valid, false otherwise
     */
    bool validate() const;

    /**
     * Throw std::runtime_error if invalid.
     * Used at API boundaries to catch errors early.
     *
     * @param context Error message prefix (e.g., "IOVector copyFrom destination")
     * @throw std::runtime_error if invalid
     */
    void validateOrThrow(const char* context) const;

    // ========== Accessors ==========

    /// Logical length (frames)
    length_t length() const { return length_; }

    /// Number of pages backing this vector
    size_t pageCount() const { return pages_.size(); }

    /// Get the i-th page (range-checked; throws if out of range)
    std::shared_ptr<twOutputPage> pageAt(size_t index) const;

    /// Starting offset within first page
    offset_t startOffset() const { return startOffset_; }

    /// Available frames from startOffset to end of backing pages
    length_t availableFrames() const;

    /**
     * Raw pointer to first page's sample buffer (if single-page; else throw).
     * Use only for legacy interop or when buffer is guaranteed single-page.
     *
     * @return Pointer to first page's samples array
     * @throw std::runtime_error if multi-page or invalid
     */
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
     * - Handles multi-page: if source spans multiple pages, handles correctly
     * - Partial copy OK: if only N < numFrames are available, copies N
     * - No-throw: returns 0 if source/dest invalid (after validate() check)
     *
     * Semantics: this[dstOffset + i] = source[srcOffset + i] for i in [0, numFrames)
     */
    length_t copyFrom(const IOVector& source,
                      offset_t srcOffset,
                      length_t numFrames);

    /**
     * Copy samples from this IOVector (source) into destination.
     * Symmetric to copyFrom(); useful for clarity in different contexts.
     *
     * @param dest       Destination IOVector (will be modified)
     * @param dstOffset  Write position within dest
     * @param numFrames  How many frames to copy
     * @return           Frames actually copied
     *
     * Semantics: dest[dstOffset + i] = this[srcOffset + i]
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
     * Properties:
     * - Bounds-safe: validates offsets and lengths
     * - Accumulating: adds (not replaces) source samples
     * - Hot-path optimized: minimal overhead
     *
     * Semantics: this[dstOffset + i] += source[i] for i in [0, numFrames)
     */
    length_t mixFrom(const IOVector& source,
                     offset_t dstOffset,
                     length_t numFrames);

    /**
     * Fill this IOVector with silence (zero samples).
     * Useful for initialization and padding.
     *
     * @param dstOffset Start offset
     * @param numFrames Number of frames to zero
     * @return          Frames actually filled
     */
    length_t fillSilence(offset_t dstOffset, length_t numFrames);

    /**
     * Slice this IOVector: create a new view starting at offset
     * with a subset of the data (shallow; no copy).
     *
     * Useful for sub-operations (e.g., mixing a segment of a clip).
     *
     * @param offset Offset from start of this vector
     * @param length Logical length of slice (clamped to available)
     * @return       New IOVector view
     */
    IOVector slice(offset_t offset, length_t length) const;

    // ========== Debugging ==========

    /**
     * Human-readable description for debugging.
     * Example: "IOVector(1 page, off=0, len=65536, avail=65536)"
     */
    std::string describe() const;

private:
    std::vector<std::shared_ptr<twOutputPage>> pages_;
    offset_t startOffset_;
    length_t length_;

    // Internal helper: map logical offset to (page_index, offset_in_page)
    struct LogicalToPhysical {
        size_t pageIndex;
        offset_t offsetInPage;
    };

    /**
     * Map a logical offset (relative to startOffset_) to physical page index
     * and offset within that page. Returns {0, 0} if out of bounds.
     */
    LogicalToPhysical mapOffset(offset_t logical) const;

    /**
     * Clamp length to available space starting from given logical offset.
     */
    length_t clampLength(offset_t logicalOffset, length_t requested) const;
};

#endif
