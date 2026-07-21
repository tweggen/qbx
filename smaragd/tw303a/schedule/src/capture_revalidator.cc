#include "tw/schedule/capture_revalidator.h"
#include "tw/pages/capture_page_pool.h"
#include "tw/core/twlog.h"
#include "tw/schedule/revalidatable.h"    // engine-side object contract (proposal 14, Phase 0)
#include "tw/schedule/capture_aspects.h"  // Preview / Playback / Metadata / Export bits
#include "tw/graph/twcomponent.h"  // For twComponent::freezePage() and freezePreviewPage()
#include "tw/graph/tw_frozen_inputs.h"  // Dataflow stage 3: bound input sets
#include "tw/pages/tw_output_page.h"  // For twOutputPage and twAspectAll
#include <cassert>
#include <cstring>
#include <vector>
#include <algorithm>

CaptureRevalidator::CaptureRevalidator(CapturePagePool* pagePool, int numWorkers)
    : pagePool_(pagePool) {
    assert(pagePool_);
    assert(numWorkers > 0);

    // Spawn worker threads
    for (int i = 0; i < numWorkers; ++i) {
        workers_.emplace_back([this]() { workerLoop(); });
    }
}

CaptureRevalidator::~CaptureRevalidator() {
    shutdown();
}

void CaptureRevalidator::scheduleRevalidation(IRevalidatable* object, uint32_t aspects, int priority) {
    if (!object || aspects == 0) return;

    {
        object->revalAddRef();
        std::lock_guard<std::mutex> lock(queueLock_);
        revalidationQueue_.push({object, aspects, priority});
    }
    queueNotEmpty_.notify_one();
}

void CaptureRevalidator::shutdown() {
    {
        std::lock_guard<std::mutex> lock(queueLock_);
        shutdown_ = true;

        // Abort pending graph demands so no consumer stays blocked in wait();
        // in-flight nodes finish on their own (their demand refs are on the
        // node and notified normally — a second notify is harmless).
        for (auto &kv : graphNodes_) {
            for (auto &dm : kv.second->demands) {
                std::lock_guard<std::mutex> dl(dm->m_);
                dm->aborted_ = true;
                dm->cv_.notify_all();
            }
            kv.second->demands.clear();
        }
        graphNodes_.clear();
        graphReady_.clear();
    }
    queueNotEmpty_.notify_all();

    // Join all workers
    for (auto& thread : workers_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

size_t CaptureRevalidator::jobsQueued() const {
    std::lock_guard<std::mutex> lock(queueLock_);
    return revalidationQueue_.size() + graphReady_.size();
}

CaptureRevalidator::GraphStats CaptureRevalidator::graphStats() const {
    GraphStats s;
    s.nodesExecuted = statNodesExecuted_.load();
    s.nodeRetries   = statNodeRetries_.load();
    s.missPages     = statMissPages_.load();
    return s;
}

void CaptureRevalidator::workerLoop() {
    while (true) {
        CaptureRevalidationJob revalJob;
        std::shared_ptr<PageNode> graphNode;
        bool haveReval = false;

        {
            std::unique_lock<std::mutex> lock(queueLock_);
            // Wait for work — but not while paused (an offline render owns the
            // graph then; see pause()). While only BACKGROUND work is paused
            // (pauseBackground(), a scheduler-driven render in progress),
            // graph nodes still count as work. Still wake for shutdown.
            queueNotEmpty_.wait(lock, [this]() {
                return shutdown_ ||
                    (!paused_ &&
                     ((!backgroundPaused_ && !revalidationQueue_.empty())
                      || !graphReady_.empty()));
            });

            // If shutting down and all queues empty, exit
            if (shutdown_ && revalidationQueue_.empty() && graphReady_.empty()) {
                break;
            }

            // Paused (woke only for a shutdown check) or nothing to do: loop.
            const bool backgroundAvailable =
                !backgroundPaused_ && !revalidationQueue_.empty();
            if (paused_ || (!backgroundAvailable && graphReady_.empty())) {
                continue;
            }

            // Dequeue the higher-priority job of the two sources.
            // Ties: reval first (the most UI-critical). Background (reval)
            // is masked while backgroundPaused_.
            const int INT_NONE = -2147483647;
            const int rp = (backgroundPaused_ || revalidationQueue_.empty())
                            ? INT_NONE : revalidationQueue_.top().priority;
            const int gp = graphReady_.empty() ? INT_NONE
                            : graphReady_.front()->priority;

            if (rp != INT_NONE && rp >= gp) {
                revalJob = revalidationQueue_.top();
                revalidationQueue_.pop();
                haveReval = true;
            } else if (gp != INT_NONE) {
                graphNode = graphReady_.front();
                graphReady_.pop_front();
            }

            // Mark in-flight under the lock so pause() can drain reliably;
            // background jobs also count separately for pauseBackground().
            if (haveReval || graphNode) ++activeJobs_;
            if (haveReval) ++activeBackgroundJobs_;
            // Record the specific object so retireObject() can block on just it
            // (the reval queue holds borrowed IRevalidatable*; graph nodes hold
            // shared_ptr and are already lifetime-safe).
            if (haveReval && revalJob.object) ++activeRevalObjects_[revalJob.object];
        }

        // Process outside the lock.
        if (haveReval) {
            processRevalidationJob(revalJob);
        } else if (graphNode) {
            processGraphNode(graphNode);
        }

        if (haveReval || graphNode) {
            {
                std::lock_guard<std::mutex> lock(queueLock_);
                --activeJobs_;
                if (haveReval) --activeBackgroundJobs_;
                if (haveReval && revalJob.object) {
                    auto it = activeRevalObjects_.find(revalJob.object);
                    if (it != activeRevalObjects_.end() && --it->second <= 0)
                        activeRevalObjects_.erase(it);
                }
            }
            idleCv_.notify_all();  // wake a pause()/retireObject() that is draining
        }
    }
}

void CaptureRevalidator::pause() {
    std::unique_lock<std::mutex> lock(queueLock_);
    paused_ = true;
    // Block until every in-flight job has finished, so the caller owns the graph
    // alone. New jobs may still be enqueued; they run after resume().
    idleCv_.wait(lock, [this]() { return activeJobs_ == 0; });
}

void CaptureRevalidator::resume() {
    {
        std::lock_guard<std::mutex> lock(queueLock_);
        paused_ = false;
    }
    queueNotEmpty_.notify_all();
}

void CaptureRevalidator::pauseBackground() {
    std::unique_lock<std::mutex> lock(queueLock_);
    backgroundPaused_ = true;
    // Drain in-flight BACKGROUND jobs only; graph nodes keep running (the
    // caller's own render demands execute on these workers).
    idleCv_.wait(lock, [this]() { return activeBackgroundJobs_ == 0; });
}

void CaptureRevalidator::resumeBackground() {
    {
        std::lock_guard<std::mutex> lock(queueLock_);
        backgroundPaused_ = false;
    }
    queueNotEmpty_.notify_all();
}

void CaptureRevalidator::retireObject(IRevalidatable* object) {
    if (!object) return;

    std::unique_lock<std::mutex> lock(queueLock_);

    // 1. Drop every queued reval job for this object. It is being destroyed, so
    //    its pin count is moot — we intentionally do NOT revalRemoveRef() the
    //    dropped jobs (the last unpin re-arms a pending deleteLater(), which is
    //    pointless noise on an object already in its destructor). Rebuild the
    //    priority queue without the retiring object's jobs; other objects' jobs
    //    and ordering survive.
    if (!revalidationQueue_.empty()) {
        std::priority_queue<CaptureRevalidationJob> kept;
        while (!revalidationQueue_.empty()) {
            CaptureRevalidationJob j = revalidationQueue_.top();
            revalidationQueue_.pop();
            if (j.object != object) kept.push(j);
        }
        revalidationQueue_.swap(kept);
    }

    // 2. Block until no worker is still processing a job for this object, so no
    //    worker can dereference it after we return (and it is torn down). Workers
    //    release queueLock_ while processing, then re-take it to clear the entry
    //    and notify idleCv_.
    idleCv_.wait(lock, [this, object]() {
        return activeRevalObjects_.find(object) == activeRevalObjects_.end();
    });
}

// --- Proposal 19 dataflow stage 3: dependency-counting page scheduler --------

void CaptureRevalidator::GraphDemand::wait() {
    std::unique_lock<std::mutex> l(m_);
    cv_.wait(l, [this]() { return outstanding_ == 0 || aborted_; });
}

bool CaptureRevalidator::GraphDemand::done() const {
    std::lock_guard<std::mutex> l(m_);
    return outstanding_ == 0;
}

std::shared_ptr<CaptureRevalidator::GraphDemand>
CaptureRevalidator::requestGraphPages(std::shared_ptr<twComponent> root,
                                      offset_t startPos, int nPages,
                                      int priority) {
    auto demand = std::make_shared<GraphDemand>();
    demand->priority_ = priority;
    if (!root || nPages <= 0 || shutdown_) return demand;   // vacuously done

    // Serialize expansions; planPage() below takes component mutexes, so
    // queueLock_ is only ever taken in short bursts inside expandNode_.
    std::lock_guard<std::mutex> ex(expansionMutex_);

    const uint64_t cap = twOutputPage::FRAME_CAPACITY;
    std::vector<std::shared_ptr<PageNode>> roots;
    for (int i = 0; i < nPages; ++i) {
        auto n = expandNode_(root, startPos + (uint64_t)i * cap, priority, 0);
        if (n) roots.push_back(n);
    }

    {
        std::lock_guard<std::mutex> lock(queueLock_);
        for (auto &n : roots) {
            if (n->state == PageNode::Done) continue;
            n->demands.push_back(demand);
            // No demand->m_ needed: the handle is not published to any waiter
            // until we return it.
            ++demand->outstanding_;
        }
    }
    queueNotEmpty_.notify_all();
    return demand;
}

// Expand one (component, pageStart) node: plan it, expand its deps, wire the
// dependency counters, and enqueue it if already runnable. Caller holds
// expansionMutex_; queueLock_ is taken only in short bursts (see the
// expansionMutex_ declaration for the lock-order rationale).
std::shared_ptr<CaptureRevalidator::PageNode>
CaptureRevalidator::expandNode_(std::shared_ptr<twComponent> comp,
                                offset_t pageStart, int priority, int depth) {
    if (!comp || depth > 32) return nullptr;   // depth guard (cyclic graphs
                                               // are excluded by FreezeContext
                                               // at render; this guards the
                                               // structural walk)

    const NodeKey key{comp.get(), pageStart};
    {
        std::lock_guard<std::mutex> lock(queueLock_);
        auto it = graphNodes_.find(key);
        if (it != graphNodes_.end()) return it->second;   // dedup, any state
    }

    // Plan OUTSIDE queueLock_ (takes the component's own mutex).
    twPagePlan plan = comp->planPage(pageStart);

    auto node = std::make_shared<PageNode>();
    node->component = std::move(comp);
    node->pageStart = pageStart;
    node->priority = priority;
    node->plan = std::move(plan);

    // Children first (recursion still under expansionMutex_, no queueLock_).
    std::vector<std::shared_ptr<PageNode>> childNodes;
    childNodes.reserve(node->plan.deps.size());
    for (const twPageDep &d : node->plan.deps) {
        if (auto c = expandNode_(d.producer, d.pageStart, priority, depth + 1))
            childNodes.push_back(std::move(c));
    }

    {
        std::lock_guard<std::mutex> lock(queueLock_);
        auto inserted = graphNodes_.emplace(key, node);
        if (!inserted.second) return inserted.first->second;   // paranoia

        for (auto &c : childNodes) {
            node->deps.push_back(c);
            if (c->state != PageNode::Done) {
                ++node->pendingDeps;
                c->dependents.push_back(node);
            }
        }
        // Predecessor edge: in-position-order execution + DSP state chaining
        // for the SAME component, when its previous page is in flight.
        if (pageStart >= (uint64_t)twOutputPage::FRAME_CAPACITY) {
            auto it = graphNodes_.find(
                NodeKey{node->component.get(),
                        pageStart - twOutputPage::FRAME_CAPACITY});
            if (it != graphNodes_.end()) {
                node->predecessor = it->second;
                if (it->second->state != PageNode::Done) {
                    ++node->pendingDeps;
                    it->second->dependents.push_back(node);
                }
            }
        }
        if (node->pendingDeps == 0) {
            node->state = PageNode::Ready;
            graphReady_.push_back(node);
        }
    }
    queueNotEmpty_.notify_one();
    return node;
}

// Execute one READY node: bind the deps' pages, render via the virtual freeze
// path, verify-at-publish (one bounded retry), then complete.
void CaptureRevalidator::processGraphNode(const std::shared_ptr<PageNode> &node) {
    {
        std::lock_guard<std::mutex> lock(queueLock_);
        node->state = PageNode::Running;
    }

    // Deps are Done (guaranteed by the counters); their results are immutable.
    twFrozenInputs inputs;
    for (auto &d : node->deps)
        if (d->result) inputs.bind(d->component.get(), d->result);
    std::shared_ptr<twOutputPage> prev =
        node->predecessor ? node->predecessor->result : nullptr;

    std::shared_ptr<twOutputPage> page =
        node->component->freezePageWithInputs(node->pageStart, inputs, prev);

    // Verify-at-publish: dep pages still current AND plan complete. One
    // bounded retry with freshly frozen deps; content correctness holds
    // regardless (the stage-2 legacy fallback inside the render), the retry
    // improves cache quality after a mid-render edit.
    bool staleDep = false;
    for (auto &d : node->deps) {
        if (d->result &&
            d->result->contentEpoch.load() < d->component->contentEpochNow()) {
            staleDep = true;
            break;
        }
    }
    statMissPages_.fetch_add(inputs.misses.size(), std::memory_order_relaxed);
    if ((staleDep || !inputs.misses.empty()) && node->attempts++ < 1) {
        statNodeRetries_.fetch_add(1, std::memory_order_relaxed);
        twFrozenInputs fresh;
        for (auto &d : node->deps) {
            auto p = d->component->requestPage(
                d->pageStart, nullptr, 0,
                (length_t)twOutputPage::FRAME_CAPACITY, 0, nullptr);
            if (p) fresh.bind(d->component.get(), p);
        }
        page = node->component->freezePageWithInputs(node->pageStart, fresh, prev);
    }
    statNodesExecuted_.fetch_add(1, std::memory_order_relaxed);

    completeGraphNode(node, std::move(page));
}

void CaptureRevalidator::completeGraphNode(const std::shared_ptr<PageNode> &node,
                                           std::shared_ptr<twOutputPage> page) {
    std::vector<std::shared_ptr<GraphDemand>> toNotify;
    {
        std::lock_guard<std::mutex> lock(queueLock_);
        node->result = std::move(page);
        node->state = PageNode::Done;
        for (auto &wd : node->dependents) {
            if (auto dep = wd.lock()) {
                if (dep->state == PageNode::Waiting && --dep->pendingDeps == 0) {
                    dep->state = PageNode::Ready;
                    graphReady_.push_back(dep);
                }
            }
        }
        node->dependents.clear();
        toNotify.swap(node->demands);
        // Done nodes leave the dedup map immediately: their pages live on in
        // the components' own caches; dependents still hold their shared_ptrs
        // until they run. A future demand re-plans and cache-hits instead.
        graphNodes_.erase(NodeKey{node->component.get(), node->pageStart});
    }
    queueNotEmpty_.notify_all();

    for (auto &dm : toNotify) {
        std::lock_guard<std::mutex> dl(dm->m_);
        if (--dm->outstanding_ == 0) dm->cv_.notify_all();
    }
}

void CaptureRevalidator::processRevalidationJob(const CaptureRevalidationJob& job) {
    assert(job.object);
    assert(job.aspects != 0);

    std::shared_ptr<CapturePageData> nextPage;

    // === CRITICAL SECTION 1: Check state and allocate page ===
    {
        std::unique_lock<std::mutex> lock(job.object->revalMutex());

        // Check if object still needs revalidation (state may have changed while queued)
        // _nolock: we hold the lock (std::lock_guard above)
        if (!job.object->revalNeeded_nolock(job.aspects)) {
            // Balance the revalAddRef() from scheduleRevalidation() on EVERY exit
            // path (this early-out used to leak a ref, over-holding the object).
            lock.unlock();
            job.object->revalRemoveRef();
            return;
        }

        // Get or allocate nextPage (under lock)
        nextPage = job.object->revalGetNextPage_nolock();
        if (!nextPage) {
            nextPage = pagePool_->allocatePage();
            if (!nextPage) {
                // Pool exhausted: re-queue at lowest priority and skip for now
                scheduleRevalidation(job.object, job.aspects, 0);
                // Unlock before removing reference.
                lock.unlock();
                job.object->revalRemoveRef();
                return;
            }
            job.object->revalSetNextPage_nolock(nextPage);
        }
    }  // Release lock before potentially long-running recomputation

    // === REVALIDATE (blocking, OUTSIDE locks) ===
    // This may take 10-100ms depending on aspect complexity
    // No locks held, so UI/audio threads can proceed
    // Dispatch to the object's virtual recomputation methods
    uint32_t succeeded = dispatchRecomputation(job.object, job.aspects, *nextPage);

    if (succeeded == 0) {
        job.object->revalRemoveRef();

        // Nothing recomputed (e.g. preview source not materializable yet).
        // Keep the current page as-is; nextPage stays parked on the object for
        // the retry that the next getCapture() schedules.
        return;
    }

    // === CRITICAL SECTION 2: Atomic swap and mark valid ===
    {
        std::lock_guard<std::mutex> objectLock(job.object->revalMutex());
        job.object->revalSwapPages_nolock();

        // Now currentPage is nextPage. Update its metadata while holding page lock
        // to prevent concurrent reads from seeing torn updates.
        std::lock_guard<std::mutex> pageLock(nextPage->pageMutex);
        nextPage->validAspects |= succeeded;
        nextPage->generation++;
    }

    // Phase 5e.6: notify the object (no locks held) so it can queue a UI
    // repaint — without this, a preview that lands while the app is idle
    // stays invisible until some unrelated repaint happens.
    job.object->revalCompleted(succeeded);
    job.object->revalRemoveRef();
}

uint32_t CaptureRevalidator::dispatchRecomputation(IRevalidatable* object, uint32_t aspects, CapturePageData& page) {
    // Phase 5: Unified rendering dispatch
    // Preview now uses freezePreviewPage() pipeline (same as playback);
    // Metadata/Export still use legacy recomputeXXX for now.
    uint32_t succeeded = 0;

    if (aspects & Preview) {
        // Let the object materialize its preview source first (e.g. a container
        // cut's offline capture). On failure the Preview aspect stays stale so
        // the next getCapture() retries once the source becomes available.
        if (!object->revalPrepPreview()) {
            TW_LOGE( "schedule", "[PREVIEW] prep failed for obj=%p; preview stays stale for retry",
                    (void*)object );
            aspects &= ~Preview;
        }
    }

    if (aspects & Preview) {
        // Phase 5: Use freezePreviewPage() for preview rendering
        // Provides lower-resolution waveform data via unified DSP pipeline.
        // Falls back to previous page if new page not ready (non-blocking).

        std::shared_ptr<twComponent> rootComp = object->revalRootComponent();

        // Preview rendering parameters:
        // - previewSampleRate: 1000 Hz typical for waveform visualization
        // - fullSampleRate: actual component rate for state consistency
        // - previousPage: current page for fallback if new page not ready
        int previewRate = 1000;  // Configurable: lower = coarser visualization
        int fullRate = 48000;    // TODO: get from environment

        auto frozenPage = rootComp->freezePreviewPage(
            0,                       // startPos: 0 for full preview
            page.PAGE_SIZE / sizeof(float),  // length: full page capacity
            previewRate,
            fullRate,
            nullptr                  // No previous page (revalidator manages fallback)
        );

        TW_LOGD( "schedule", "[PREVIEW] recompute obj=%p comp=%p -> %s validFrames=%u",
                (void*)object, (void*)rootComp.get(),
                frozenPage ? "page" : "NULL",
                frozenPage ? frozenPage->validFrames : 0u );

        if( frozenPage && frozenPage->validFrames > 0 ) {
            // Copy frozen preview samples into CapturePageData
            size_t samplesToCopy = std::min((size_t)frozenPage->validFrames, (size_t)page.PAGE_SIZE / sizeof(float));
            if( samplesToCopy > 0 ) {
                memcpy(page.data, frozenPage->samples.data(), samplesToCopy * sizeof(float));
                page.validAspects |= Preview;
                succeeded |= Preview;
            }
        }
    }

    if (aspects & Metadata) {
        object->revalRecomputeMetadata(page);
        succeeded |= Metadata;
    }
    if (aspects & Export) {
        object->revalRecomputeExport(page);
        succeeded |= Export;
    }

    // Aspects requested but not handled above (e.g. Playback) keep the
    // historical behavior of being marked valid by the caller.
    succeeded |= (aspects & ~(Preview | Metadata | Export));

    return succeeded;
}

// (Dataflow stage 6: processComponentFreezingJob and its queue were retired —
// zero callers; page pre-computation is the graph scheduler's job.)
