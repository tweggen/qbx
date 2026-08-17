#ifndef CAPTURE_PAGE_POOL_H
#define CAPTURE_PAGE_POOL_H

#include <cstdint>
#include "tw/core/twtypes.h"
#include <vector>
#include <queue>
#include <memory>
#include <mutex>
#include <any>
#include <chrono>
#include "tw/pages/tw_output_page.h"  // For FROZEN_PAGE_SIZE_BYTES
#include "tw/pages/page_interface.h"

/**
 * Capture page pool: pre-allocated, fixed-size pages for audio capture data.
 *
 * Design (Unix page cache model):
 * - Pre-allocate N pages of fixed size (256kB each, typically)
 * - Custom deleter for shared_ptr: returns pages to pool instead of deallocating
 * - Thread-safe allocation/deallocation with minimal contention
 * - Memory fragmentation avoided: pages are pre-allocated contiguous buffer
 *
 * Usage:
 *   auto pool = std::make_unique<CapturePagePool>(2048);  // 512MB
 *   auto page = pool->allocatePage();  // O(1), may return nullptr if exhausted
 *   // ... use page ...
 *   page.reset();  // Custom deleter returns it to pool
 */

/**
 * A single capture page: holds audio data + metadata.
 *
 * Size: 256kB (power of 2, aligned for efficiency)
 * - ~0.68 seconds at 48kHz stereo float32
 * - Aligns nicely with typical I/O buffer sizes (4096, 8192, etc.)
 *
 * Content varies by aspect:
 * - Preview: FLOAT SAMPLES of the object's output, decimated to ~1 kHz,
 *   CHANNEL 0 ONLY, starting at position 0, with NO geometry attached (no probe
 *   count, no hop, no duration, no channel count). Written by exactly one
 *   place, CaptureRevalidator::dispatchRecomputation, from
 *   twComponent::freezePreviewPage.
 *   NOTE (proposal 36 B9): "channel 0 only" is still literally true and is NOT
 *   the same statement as main/model/CONTRACT.md inv. 9a, which says a preview
 *   PROBE folds every channel (B8). They do not conflict because this payload
 *   has ZERO READERS — B8 deleted the only one as dead code (trap 26). The
 *   drawn waveform comes from SObject::straightCalcPreviewData /
 *   SCut::ensureCapturePeaks, which do fold every channel; this page's
 *   existence is used as a readiness signal and its bytes are never read.
 * - Playback: reader chain data (twSampleReader with grain params, etc.)
 * - Metadata: duration, peak levels, RMS
 * - Export: resampled/normalized buffer
 *
 * The Preview line above used to read "waveform peaks (small, resampled)", and
 * that wording is what proposal 36 trap 26 was: it invited a reader to treat
 * `data` as an array of preview_t {int8 min, int8 max} probes, which is what
 * SPlainWave::getPreview did (unreachably -- see the note there). It is NOT
 * that. The waveform probe array is a different thing entirely, computed by
 * SObject::straightCalcPreviewData / SCut::ensureCapturePeaks and persisted in
 * the "preview.peaks" sidecar; it never travels through a CapturePageData.
 *
 * A Preview aspect page's ONLY consumer today is SCut::getPreview, which uses
 * its EXISTENCE as a readiness signal and never reads `data`. Nothing in the
 * tree reads the float payload. Before giving it a reader, give it a geometry.
 *
 * NOT WIDENED BY PROPOSAL 36 (settled at B7, restated at B8): this type has no
 * frame, stride or channel field, and nothing on the audio path allocates one.
 * A page's audio width lives in twOutputPage::channels(), a different type.
 */
struct CapturePageData : public PageBase {
    // Phase 5 Gap 11: Unified page size with twOutputPage
    static constexpr size_t PAGE_SIZE = FROZEN_PAGE_SIZE_BYTES;  // 256 kB

    // Synchronization: protects concurrent read/write of data and metadata.
    // Revalidator holds this lock while writing to data/validAspects.
    // UI readers acquire this lock when reading validAspects or data pointers.
    // Kept first (before data) to ensure good cache alignment of lock.
    mutable std::mutex pageMutex;

    // Actual page data (aligned for cache efficiency)
    alignas(4096) uint8_t data[PAGE_SIZE];

    // Metadata (kept in-struct for locality)
    uint32_t validAspects = 0;      // Bitmask: which aspects are computed in this page
    int generation = 0;             // Incremented on each revalidation (for staleness tracking)
    offset_t startPosition = 0;     // Time position where page begins
    std::any internalState;         // Internal state snapshot (for sequential components)
    std::chrono::steady_clock::time_point createdAt;  // Creation time for staleness tracking

    // Constructor: initialize to empty
    CapturePageData()
        : validAspects(0), generation(0), startPosition(0),
          createdAt(std::chrono::steady_clock::now()) {}

    // PageBase interface implementation
    std::mutex& getMutex() const override { return pageMutex; }
    offset_t getStartPosition() const override { return startPosition; }
    void setStartPosition(offset_t pos) override { startPosition = pos; }
    uint32_t getValidAspects() const override { return validAspects; }
    void setValidAspects(uint32_t aspects) override { validAspects = aspects; }
    uint64_t getGeneration() const override { return generation; }
    void incrementGeneration() override { generation++; }
    size_t getPageSize() const override { return PAGE_SIZE; }
    uint32_t getValidFrames() const override { return PAGE_SIZE / sizeof(float); }
    void setValidFrames(uint32_t /* frames */) override {}  // CapturePageData uses full buffer
    // getDataPtr() is GONE (proposal 36 B9). `data` is a public member of a
    // plain struct; the accessor added nothing but a polymorphic hole.
    std::any& getInternalState() override { return internalState; }
    const std::any& getInternalState() const override { return internalState; }
    std::chrono::steady_clock::time_point getCreatedAt() const override { return createdAt; }
};

/**
 * Thread-safe pool of pre-allocated capture pages.
 *
 * Manages allocation/deallocation with zero fragmentation:
 * - Pages stored in one contiguous vector
 * - Free indices stored in queue
 * - Custom deleter (via shared_ptr) returns pages to free queue
 */
class CapturePagePool {
public:
    /**
     * Custom deleter for shared_ptr: returns page to pool instead of delete.
     *
     * Usage:
     *   std::shared_ptr<CapturePageData> page(pagePtr, PageDeleter{this, pageIndex});
     */
    struct PageDeleter {
        CapturePagePool* pool;
        size_t pageIndex;

        void operator()(CapturePageData* page) {
            pool->releasePage(pageIndex);
        }
    };

    /**
     * Construct pool with N pre-allocated pages.
     *
     * @param numPages Number of 256kB pages to allocate
     *                 E.g., 2048 = 512MB total
     */
    explicit CapturePagePool(size_t numPages = 2048);

    ~CapturePagePool();

    /**
     * Allocate a page from the pool (O(1)).
     *
     * @return shared_ptr with custom deleter that returns page to pool on delete
     *         nullptr if pool exhausted
     *
     * Thread-safe: multiple threads can allocate concurrently.
     *
     * Usage:
     *   if (auto page = pool->allocatePage()) {
     *       // Use page
     *       page->validAspects = Playback;
     *       // ... fill page->data ...
     *   } else {
     *       // Pool exhausted; fallback to stale data
     *   }
     */
    std::shared_ptr<CapturePageData> allocatePage();

    /**
     * Release a page back to the pool.
     *
     * Called by PageDeleter; not meant for direct use.
     * Clears valid aspects to mark page as empty before returning to free list.
     */
    void releasePage(size_t pageIndex);

    /**
     * Get pool statistics (for diagnostics).
     */
    size_t numPages() const { return pages_.size(); }
    size_t numFreePages() const;
    size_t numAllocatedPages() const;

private:
    // Pre-allocated pages (huge contiguous buffer)
    std::vector<CapturePageData> pages_;

    // Queue of free page indices
    std::queue<size_t> freeIndices_;

    // Thread safety
    mutable std::mutex poolLock_;
};

#endif  // CAPTURE_PAGE_POOL_H
