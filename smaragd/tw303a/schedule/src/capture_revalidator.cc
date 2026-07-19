#include "tw/schedule/capture_revalidator.h"
#include "tw/pages/capture_page_pool.h"
#include "tw/schedule/revalidatable.h"    // engine-side object contract (proposal 14, Phase 0)
#include "tw/schedule/capture_aspects.h"  // Preview / Playback / Metadata / Export bits
#include "tw/graph/twcomponent.h"  // For twComponent::freezePage() and freezePreviewPage()
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

void CaptureRevalidator::scheduleComponentFreezing(std::shared_ptr<twComponent> component, uint64_t pageStartPos,
                                                    std::shared_ptr<twOutputPage> previousPage,
                                                    int priority) {
    if (!component) return;

    {
        std::lock_guard<std::mutex> lock(queueLock_);
        freezingQueue_.push({component, pageStartPos, previousPage, priority});
    }
    queueNotEmpty_.notify_one();
}

void CaptureRevalidator::shutdown() {
    {
        std::lock_guard<std::mutex> lock(queueLock_);
        shutdown_ = true;
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
    return revalidationQueue_.size() + freezingQueue_.size();
}

void CaptureRevalidator::workerLoop() {
    while (true) {
        CaptureRevalidationJob revalJob;
        ComponentFreezingJob freezeJob;
        bool haveReval = false, haveFreeze = false;

        {
            std::unique_lock<std::mutex> lock(queueLock_);
            // Wait for work — but not while paused (an offline render owns the
            // graph then; see pause()). Still wake for shutdown.
            queueNotEmpty_.wait(lock, [this]() {
                return shutdown_ ||
                    (!paused_ && (!revalidationQueue_.empty() || !freezingQueue_.empty()));
            });

            // If shutting down and both queues empty, exit
            if (shutdown_ && revalidationQueue_.empty() && freezingQueue_.empty()) {
                break;
            }

            // Paused (woke only for a shutdown check) or nothing to do: loop.
            if (paused_ || (revalidationQueue_.empty() && freezingQueue_.empty())) {
                continue;
            }

            // Dequeue highest priority job from either queue
            // Prioritize revalidation jobs slightly (they're more UI-critical)
            if (!revalidationQueue_.empty() && (freezingQueue_.empty() ||
                revalidationQueue_.top().priority >= freezingQueue_.top().priority)) {
                revalJob = revalidationQueue_.top();
                revalidationQueue_.pop();
                haveReval = true;
            } else if (!freezingQueue_.empty()) {
                freezeJob = freezingQueue_.top();
                freezingQueue_.pop();
                haveFreeze = true;
            }

            // Mark in-flight under the lock so pause() can drain reliably.
            if (haveReval || haveFreeze) ++activeJobs_;
            // Record the specific object so retireObject() can block on just it
            // (the reval queue holds borrowed IRevalidatable*; the freeze queue
            // holds shared_ptr and is already lifetime-safe).
            if (haveReval && revalJob.object) ++activeRevalObjects_[revalJob.object];
        }

        // Process outside the lock.
        if (haveReval) {
            processRevalidationJob(revalJob);
        } else if (haveFreeze) {
            processComponentFreezingJob(freezeJob);
        }

        if (haveReval || haveFreeze) {
            {
                std::lock_guard<std::mutex> lock(queueLock_);
                --activeJobs_;
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

void CaptureRevalidator::retireObject(IRevalidatable* object) {
    if (!object) return;

    std::unique_lock<std::mutex> lock(queueLock_);

    // 1. Drop every queued reval job for this object. It is being destroyed, so
    //    its reference count is moot — we intentionally do NOT revalRemoveRef()
    //    the surviving jobs (that would re-enter removeRef()/deleteLater() on an
    //    object already in its destructor). Rebuild the priority queue without
    //    the retiring object's jobs; other objects' jobs and ordering survive.
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
            fprintf(stderr, "[PREVIEW] prep failed for obj=%p; preview stays stale for retry\n",
                    (void*)object);
            fflush(stderr);
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

        fprintf(stderr, "[PREVIEW] recompute obj=%p comp=%p -> %s validFrames=%u\n",
                (void*)object, (void*)rootComp.get(),
                frozenPage ? "page" : "NULL",
                frozenPage ? frozenPage->validFrames : 0u);
        fflush(stderr);

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

void CaptureRevalidator::processComponentFreezingJob(const ComponentFreezingJob& job) {
    // Phase 4 Gap 10: Component-level page freezing
    // Worker thread pre-computes frozen output pages for efficient rendering.
    //
    // Sequential rendering chain:
    //   freezePage(0, nullptr) → render with reset() → capture state → page 0
    //   freezePage(1, page0)   → restore state from page 0 → render → capture new state → page 1
    //   freezePage(2, page1)   → resume from page 1 → render → capture new state → page 2
    //   ...
    //
    // No locks needed on component during freezing (read-only state from latches).
    // Page pool manages memory; no object-level page swapping like SObject has.

    assert(job.component);

    // Materialize output via requestPage() (Proposal 19 Phase 2a): dedups
    // against any concurrent worker/driver freezing the same page, then
    // internally orchestrates reset/restore → renderFrames() → capture state.
    auto frozenPage = job.component->requestPage(
        job.pageStartPos,
        nullptr,                   // No pre-prepared input; renderFrames uses latches
        0,
        0,
        0,                          // sampleRate (unused in current implementation)
        job.previousPage            // Sequential state chain
    );

    if (!frozenPage) {
        return;  // Failed to freeze (rare; component may not be ready)
    }

    // Page is now frozen. Store it in component's page cache via setPageAsFrozen().
    // This allows render loops (or other consumers) to read from the frozen page
    // instead of calling calcOutputTo() redundantly.
    job.component->setPageAsFrozen(job.pageStartPos, frozenPage, twAspectAll);
}
