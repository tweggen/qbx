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
#include <variant>
#include "capture_page_pool.h"

// Forward declarations
class CapturePagePool;
class IRevalidatable;
class twComponent;
class twOutputPage;

/**
 * Component freezing job: request to freeze a twComponent output page.
 * Phase 4 Gap 10: Pre-compute component pages for efficient rendering.
 *
 * Each component freezing job specifies:
 * - Which component to freeze
 * - Which time position to freeze (page start position)
 * - Previous page (for internal state resumption)
 * - Priority (higher = process first)
 */
struct ComponentFreezingJob {
    twComponent* component;      // Which component to freeze (borrowed pointer)
    uint64_t pageStartPos;       // Position to freeze (in component sample rate)
    std::shared_ptr<twOutputPage> previousPage;  // For state chain (may be nullptr for page 0)
    int priority;                // 0-10 (higher first)

    // For priority queue: higher priority dequeues first
    bool operator<(const ComponentFreezingJob& other) const {
        return priority < other.priority;
    }
};

/**
 * Revalidation job: request to recompute specific capture aspects for a revalidatable object.
 *
 * Each job specifies:
 * - Which object needs revalidation
 * - Which aspects (Preview, Playback, Metadata, Export)
 * - Priority (higher = process first)
 *
 * Used in priority queue: higher priority jobs dequeue first.
 */
struct CaptureRevalidationJob {
    IRevalidatable* object; // Which object to revalidate (borrowed pointer)
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
 * 2. Recompute aspects into nextPage by calling object's virtual methods (blocking, expected 10-100ms per job)
 * 3. Atomic swap: object->swapPages_nolock() (object->nextPage ← nextPage, nextPage → currentPage)
 * 4. Mark aspects valid in the swapped page
 * 5. Emit revalidationComplete signal (for UI redraw)
 *
 * Thread safety:
 * - Job queue protected by mutex
 * - Each object protected by its own mutex (via IRevalidatable::revalMutex())
 * - No shared state between workers (each processes one job at a time)
 *
 * Works with any IRevalidatable (the app's SObject implements it;
 * the engine has no app knowledge — proposal 14, Phase 0).
 * Each object type implements recomputePreview(), recomputePlayback(), etc.
 * The revalidator dispatches to the appropriate method.
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
     * Schedule a revalidation job (object-level).
     *
     * Non-blocking: returns immediately, job runs in background.
     * If queue is full or pool exhausted, job may be skipped (acceptable).
     *
     * @param object Which object to revalidate (borrowed pointer)
     * @param aspects Which aspects (bitmask)
     * @param priority Job priority (higher = process first)
     *                 Suggested: 10=Playback, 5=Metadata, 2=Export, 1=Preview
     */
    void scheduleRevalidation(IRevalidatable* object, uint32_t aspects, int priority = 5);

    /**
     * Schedule a component freezing job (Phase 4 Gap 10).
     *
     * Non-blocking: pre-computes frozen component output pages for efficient rendering.
     * Workers call component->freezePage() to materialize output.
     *
     * @param component Which component to freeze (borrowed pointer)
     * @param pageStartPos Time position to freeze (in component sample rate)
     * @param previousPage Previous page's snapshot (for state resumption), may be nullptr
     * @param priority Job priority (higher = process first)
     *                 Suggested: 10=Playback, 5=Metadata, 2=Export, 1=Preview
     */
    void scheduleComponentFreezing(twComponent* component, uint64_t pageStartPos,
                                   std::shared_ptr<twOutputPage> previousPage = nullptr,
                                   int priority = 5);

    /**
     * Graceful shutdown: wait for workers to finish and join.
     *
     * Called by destructor; can also be called explicitly.
     * After shutdown, no new jobs are accepted.
     */
    void shutdown();

    /**
     * Get pool statistics (for diagnostics).
     * Returns total of both revalidation and freezing jobs.
     */
    size_t jobsQueued() const;

private:
    // Implementation: worker thread loop
    void workerLoop();

    // Helper: process a single revalidation job
    void processRevalidationJob(const CaptureRevalidationJob& job);

    // Helper: process a single component freezing job
    void processComponentFreezingJob(const ComponentFreezingJob& job);

    // Dispatch recomputation to object's virtual methods
    // These no longer do the computation themselves; they delegate to the object
    void dispatchRecomputation(IRevalidatable* object, uint32_t aspects, CapturePageData& page);

    // Members
    CapturePagePool* pagePool_;  // Borrowed, not owned

    // Job queues with priority (mutable for const methods like jobsQueued())
    mutable std::mutex queueLock_;
    std::priority_queue<CaptureRevalidationJob> revalidationQueue_;     // object jobs
    std::priority_queue<ComponentFreezingJob> freezingQueue_;           // Component jobs
    std::condition_variable queueNotEmpty_;

    // Shutdown coordination
    std::atomic<bool> shutdown_{false};

    // Worker threads
    std::vector<std::thread> workers_;
};

#endif  // CAPTURE_REVALIDATOR_H
