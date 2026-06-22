#ifndef CAPTURE_REVALIDATOR_H
#define CAPTURE_REVALIDATOR_H

#include <cstdint>
#include <queue>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <atomic>
#include "capture_page_pool.h"

// Forward declarations
class CapturePagePool;
class SCut;

/**
 * Revalidation job: request to recompute specific capture aspects for a cut.
 *
 * Each job specifies:
 * - Which cut needs revalidation
 * - Which aspects (Preview, Playback, Metadata, Export)
 * - Priority (higher = process first)
 *
 * Used in priority queue: higher priority jobs dequeue first.
 */
struct CaptureRevalidationJob {
    SCut* cut;              // Which cut to revalidate (borrowed pointer)
    uint32_t aspects;       // Which aspects (bitmask: Preview|Playback|Metadata|Export)
    int priority;           // 0-10 (higher first)

    // For priority queue: higher priority dequeues first
    bool operator<(const CaptureRevalidationJob& other) const {
        // std::priority_queue uses operator< to order (max-heap by default)
        // We want higher priority first, so invert: this < other means other comes out first
        return priority < other.priority;
    }
};

/**
 * Async capture revalidator: worker thread pool with priority job queue.
 *
 * Design:
 * - N worker threads (8 for testing, 2-4 for production)
 * - Priority job queue (Playback:10 > Metadata:5 > Export:2 > Preview:1)
 * - Non-blocking job submission (scheduleRevalidation returns immediately)
 * - Graceful shutdown (drain queue, join threads)
 *
 * Job processing:
 * 1. Allocate nextPage from pool (skip if exhausted, re-queue at low priority)
 * 2. Recompute aspects into nextPage (blocking, expected 10-100ms per job)
 * 3. Atomic swap: cut->swapPages() (cut->nextPage ← nextPage, nextPage → currentPage)
 * 4. Mark aspects valid in the swapped page
 * 5. Emit captureRevalidated(cut, aspects) signal (for UI redraw)
 *
 * Thread safety:
 * - Job queue protected by mutex
 * - Each cut protected by its own mutex (via SObject base class)
 * - No shared state between workers (each processes one job at a time)
 */
class CaptureRevalidator {
public:
    /**
     * Construct revalidator with worker thread pool.
     *
     * @param pagePool Pre-allocated page pool (not owned; must outlive revalidator)
     * @param numWorkers Number of worker threads (default: 8 for testing)
     */
    CaptureRevalidator(CapturePagePool* pagePool, int numWorkers = 8);

    /**
     * Destructor: graceful shutdown.
     * - Sets shutdown flag
     * - Wakes all workers
     * - Joins all threads
     * - Queued jobs are discarded (OK for background work)
     */
    ~CaptureRevalidator();

    /**
     * Schedule a revalidation job.
     *
     * Non-blocking: returns immediately, job runs in background.
     * If queue is full or pool exhausted, job may be skipped (acceptable).
     *
     * @param cut Which cut to revalidate (borrowed pointer)
     * @param aspects Which aspects (bitmask)
     * @param priority Job priority (higher = process first)
     *                 Suggested: 10=Playback, 5=Metadata, 2=Export, 1=Preview
     */
    void scheduleRevalidation(SCut* cut, uint32_t aspects, int priority = 5);

    /**
     * Graceful shutdown: wait for workers to finish and join.
     *
     * Called by destructor; can also be called explicitly.
     * After shutdown, no new jobs are accepted.
     */
    void shutdown();

    /**
     * Get pool statistics (for diagnostics).
     */
    size_t jobsQueued() const;

private:
    // Implementation: worker thread loop
    void workerLoop();

    // Helper: process a single revalidation job
    void processJob(const CaptureRevalidationJob& job);

    // Recompute specific aspects into the given page
    void recomputePlayback(SCut* cut, CapturePageData& page);
    void recomputeMetadata(SCut* cut, CapturePageData& page);
    void recomputePreview(SCut* cut, CapturePageData& page);
    void recomputeExport(SCut* cut, CapturePageData& page);

    // Members
    CapturePagePool* pagePool_;  // Borrowed, not owned

    // Job queue with priority (mutable for const methods like jobsQueued())
    mutable std::mutex queueLock_;
    std::priority_queue<CaptureRevalidationJob> jobQueue_;
    std::condition_variable queueNotEmpty_;

    // Shutdown coordination
    std::atomic<bool> shutdown_{false};

    // Worker threads
    std::vector<std::thread> workers_;
};

#endif  // CAPTURE_REVALIDATOR_H
