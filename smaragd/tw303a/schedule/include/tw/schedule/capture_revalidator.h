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
#include <unordered_map>
#include <map>
#include <deque>
#include "tw/pages/capture_page_pool.h"
#include "tw/graph/tw_page_plan.h"   // dataflow stage 3: node plans

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
    std::shared_ptr<twComponent> component;      // Which component to freeze (borrowed pointer)
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
    IRevalidatable* object; // Which object to revalidate (borrowed pointer), reference is added.
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
    void scheduleComponentFreezing(std::shared_ptr<twComponent> component, uint64_t pageStartPos,
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

    /**
     * Quiesce background revalidation for an exclusive operation (offline
     * render — "one player at a time"; proposal 16 "offline renders stay
     * exact"). Proposal 19 Phase 2b: an offline render pulls pages synchronously
     * and mutates shared component cursors while freezing; a background worker
     * freezing the same components concurrently corrupts the render's output
     * (the confirmed takes_group_broadcast flake). pause() stops workers from
     * dequeuing AND blocks until every in-flight job has finished, so the caller
     * then owns the graph alone. Jobs may still be enqueued while paused; they
     * run after resume(). Idempotent-safe to pair via RevalidationPause (RAII).
     */
    void pause();
    void resume();

    /**
     * Stage 4 — quiesce BACKGROUND work only. Gates the reval + component-
     * freezing queues (preview/aspect recomputation, which mutates shared
     * component state) and drains their in-flight jobs, while GRAPH DEMANDS
     * keep executing: an offline render must not race background aspect jobs
     * ("offline renders stay exact"), but its own page DAG must proceed on
     * these very workers — a full pause() here would deadlock the render's
     * demand waits. Queued background jobs run after resumeBackground().
     */
    void pauseBackground();
    void resumeBackground();

    // --- Proposal 19 dataflow stage 3: dependency-counting page scheduler ---
    //
    // A DEMAND is the consumer-facing watermark: "root pages
    // [startPos, startPos + nPages*FRAME_CAPACITY) should be frozen". The
    // scheduler expands it into (component, pageStart) NODES via
    // twComponent::planPage() (structural, no rendering), wires dependency
    // counters (input pages + the same-component predecessor page for DSP
    // state chaining), and executes READY nodes on the worker pool via
    // freezePageWithInputs() with the deps' pages bound. Nothing inside the
    // graph ever waits: a node either sits on counters or runs; workers are
    // never parked. Nodes own shared_ptrs to their components (no dangling —
    // the retireObject lesson; components are lifetime-safe by construction
    // here). Verify-at-publish: a node whose dep pages went stale mid-render
    // or whose plan proved incomplete (misses) retries once with re-frozen
    // deps; content correctness is guaranteed regardless by the stage-2
    // legacy fallback inside the render — the retry improves cache quality.
    class GraphDemand {
    public:
        // Block until every demanded root page is frozen, or the revalidator
        // shuts down (abort). NOT for the RT audio thread.
        void wait();
        bool done() const;
    private:
        friend class CaptureRevalidator;
        mutable std::mutex m_;
        std::condition_variable cv_;
        int  outstanding_ = 0;
        bool aborted_ = false;
        int  priority_ = 5;
    };

    /**
     * Demand that `root`'s pages [startPos, startPos + nPages*capacity) be
     * frozen, expanding and scheduling the whole dependency graph. Returns
     * immediately; wait on the returned handle for completion. Pages land in
     * the components' own caches (freezePage publication semantics), so any
     * consumer — including the legacy pull path — hits them.
     */
    std::shared_ptr<GraphDemand> requestGraphPages(
        std::shared_ptr<twComponent> root,
        uint64_t startPos,
        int nPages,
        int priority = 5);

    /**
     * Retire a revalidatable object that is about to be destroyed.
     *
     * The revalidation queue holds BORROWED IRevalidatable* pointers (unlike the
     * component-freezing queue, which holds shared_ptr and is lifetime-safe).
     * The object's own reference count uses Qt deleteLater(), which is a one-way
     * trip: once a delete is deferred, a later revalAddRef() from a freshly
     * scheduled job cannot rescind it, so the object can be destroyed while a job
     * that references it is still queued or in flight. A worker then dereferences
     * freed memory (e.g. locks a destroyed captureBuildMutex_ inside
     * SCut::buildCapture_ → std::mutex::lock() throws → terminate).
     *
     * Every enqueuing object MUST call this from the TOP of its destructor, while
     * it is still fully constructed: it drops all queued jobs for `object` and
     * BLOCKS until no worker is still processing a job for it. After this returns
     * the revalidator holds no reference to `object` and none will be touched.
     * Safe to call with pending or no jobs. Must NOT be called from a worker
     * thread (it would deadlock waiting on itself).
     */
    void retireObject(IRevalidatable* object);

    /**
     * RAII guard: pause() on construction (drains in-flight jobs), resume() on
     * destruction. Use around an offline render so it never races a worker.
     */
    class Guard {
    public:
        explicit Guard(CaptureRevalidator* r) : r_(r) { if (r_) r_->pause(); }
        ~Guard() { if (r_) r_->resume(); }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
    private:
        CaptureRevalidator* r_;
    };

private:
    // Implementation: worker thread loop
    void workerLoop();

    // Helper: process a single revalidation job
    void processRevalidationJob(const CaptureRevalidationJob& job);

    // Helper: process a single component freezing job
    void processComponentFreezingJob(const ComponentFreezingJob& job);

    // Dispatch recomputation to object's virtual methods.
    // These no longer do the computation themselves; they delegate to the object.
    // Returns the subset of `aspects` that was actually recomputed successfully;
    // only those get marked valid (failed aspects stay stale and retry later).
    uint32_t dispatchRecomputation(IRevalidatable* object, uint32_t aspects, CapturePageData& page);

    // Members
    CapturePagePool* pagePool_;  // Borrowed, not owned

    // Job queues with priority (mutable for const methods like jobsQueued())
    mutable std::mutex queueLock_;
    std::priority_queue<CaptureRevalidationJob> revalidationQueue_;     // object jobs
    std::priority_queue<ComponentFreezingJob> freezingQueue_;           // Component jobs
    std::condition_variable queueNotEmpty_;

    // Shutdown coordination
    std::atomic<bool> shutdown_{false};

    // Pause/drain coordination (see pause()/resume()). paused_ gates workers
    // from dequeuing; activeJobs_ counts jobs currently being processed; idleCv_
    // lets pause() block until activeJobs_ reaches 0. All guarded by queueLock_.
    bool paused_{false};
    int  activeJobs_{0};
    std::condition_variable idleCv_;

    // Background-only quiesce (see pauseBackground()): gates reval/freezing
    // dequeues and counts their in-flight jobs separately so the drain can
    // wait on exactly them while graph nodes keep running.
    bool backgroundPaused_{false};
    int  activeBackgroundJobs_{0};

    // Per-object in-flight count for retireObject(): a worker records the object
    // of the reval job it is processing here (under queueLock_) and clears it on
    // completion, so a destructor can block until no worker still touches it.
    std::unordered_map<IRevalidatable*, int> activeRevalObjects_;

    // --- Dataflow stage 3 scheduler state (all guarded by queueLock_ unless
    //     noted) -----------------------------------------------------------
    struct PageNode {
        std::shared_ptr<twComponent> component;
        uint64_t pageStart = 0;
        int priority = 5;
        int pendingDeps = 0;                 // unresolved edges (inputs + pred)
        int attempts = 0;                    // verify-at-publish retries
        enum State { Waiting, Ready, Running, Done } state = Waiting;
        std::shared_ptr<twOutputPage> result;
        twPagePlan plan;
        std::vector<std::shared_ptr<PageNode>> deps;      // owning (binding)
        std::shared_ptr<PageNode> predecessor;            // state-chain edge
        std::vector<std::weak_ptr<PageNode>> dependents;  // notify on Done
        std::vector<std::shared_ptr<GraphDemand>> demands;
    };
    using NodeKey = std::pair<const twComponent *, uint64_t>;
    std::map<NodeKey, std::shared_ptr<PageNode>> graphNodes_;  // in-flight only
    std::deque<std::shared_ptr<PageNode>> graphReady_;

    // Serializes demand expansion. planPage() takes component mutexes, so
    // expansion must NOT hold queueLock_ across it (a worker finishing a
    // render holds a component mutex and then takes queueLock_ — inversion).
    std::mutex expansionMutex_;

    std::shared_ptr<PageNode> expandNode_(std::shared_ptr<twComponent> comp,
                                          uint64_t pageStart, int priority,
                                          int depth);
    void processGraphNode(const std::shared_ptr<PageNode> &node);
    void completeGraphNode(const std::shared_ptr<PageNode> &node,
                           std::shared_ptr<twOutputPage> page);

    // Worker threads
    std::vector<std::thread> workers_;
};

#endif  // CAPTURE_REVALIDATOR_H
