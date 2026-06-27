#ifndef CAPTURE_PAGE_POOL_H
#define CAPTURE_PAGE_POOL_H

#include <cstdint>
#include <vector>
#include <queue>
#include <memory>
#include <mutex>
#include "tw_output_page.h"  // For FROZEN_PAGE_SIZE_BYTES

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
 * - Preview: waveform peaks (small, resampled)
 * - Playback: reader chain data (twSampleReader with grain params, etc.)
 * - Metadata: duration, peak levels, RMS
 * - Export: resampled/normalized buffer
 */
struct CapturePageData {
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

    // Constructor: initialize to empty
    CapturePageData() : validAspects(0), generation(0) {}
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

    ~CapturePagePool() = default;

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
