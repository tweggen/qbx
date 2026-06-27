#include "capture_revalidator.h"
#include "capture_page_pool.h"
#include "sobject.h"  // For SObject::recomputeXXX virtual methods
#include "scut.h"  // For SCutCaptureAspect enum (Preview, Playback, Metadata, Export)
#include "twcomponent.h"  // For twComponent::freezePage() and setPageAsFrozen()
#include "tw_output_page.h"  // For twOutputPage and twAspectAll
#include <cassert>
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

void CaptureRevalidator::scheduleRevalidation(SObject* object, uint32_t aspects, int priority) {
    if (!object || aspects == 0) return;

    {
        std::lock_guard<std::mutex> lock(queueLock_);
        revalidationQueue_.push({object, aspects, priority});
    }
    queueNotEmpty_.notify_one();
}

void CaptureRevalidator::scheduleComponentFreezing(twComponent* component, uint64_t pageStartPos,
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
        // Wait for job or shutdown signal
        {
            std::unique_lock<std::mutex> lock(queueLock_);
            queueNotEmpty_.wait(lock, [this]() {
                return !revalidationQueue_.empty() || !freezingQueue_.empty() || shutdown_;
            });

            // If shutting down and both queues empty, exit
            if (shutdown_ && revalidationQueue_.empty() && freezingQueue_.empty()) {
                break;
            }

            // If both queues empty (but not shutting down), loop again
            if (revalidationQueue_.empty() && freezingQueue_.empty()) {
                continue;
            }

            // Dequeue highest priority job from either queue
            // Prioritize revalidation jobs slightly (they're more UI-critical)
            if (!revalidationQueue_.empty() && (freezingQueue_.empty() ||
                revalidationQueue_.top().priority >= freezingQueue_.top().priority)) {
                CaptureRevalidationJob job = revalidationQueue_.top();
                revalidationQueue_.pop();
                lock.unlock();
                processRevalidationJob(job);
            } else if (!freezingQueue_.empty()) {
                ComponentFreezingJob job = freezingQueue_.top();
                freezingQueue_.pop();
                lock.unlock();
                processComponentFreezingJob(job);
            }
        }
    }
}

void CaptureRevalidator::processRevalidationJob(const CaptureRevalidationJob& job) {
    assert(job.object);
    assert(job.aspects != 0);

    std::shared_ptr<CapturePageData> nextPage;

    // === CRITICAL SECTION 1: Check state and allocate page ===
    {
        std::lock_guard<std::mutex> lock(job.object->mutex());

        // Check if object still needs revalidation (state may have changed while queued)
        // _nolock: we hold the lock (std::lock_guard above)
        if (!job.object->needsRevalidation_nolock(job.aspects)) {
            return;
        }

        // Get or allocate nextPage (under lock)
        nextPage = job.object->getNextPage_nolock();
        if (!nextPage) {
            nextPage = pagePool_->allocatePage();
            if (!nextPage) {
                // Pool exhausted: re-queue at lowest priority and skip for now
                scheduleRevalidation(job.object, job.aspects, 0);
                return;
            }
            job.object->setNextPage_nolock(nextPage);
        }
    }  // Release lock before potentially long-running recomputation

    // === REVALIDATE (blocking, OUTSIDE locks) ===
    // This may take 10-100ms depending on aspect complexity
    // No locks held, so UI/audio threads can proceed
    // Dispatch to the object's virtual recomputation methods
    dispatchRecomputation(job.object, job.aspects, *nextPage);

    // === CRITICAL SECTION 2: Atomic swap and mark valid ===
    {
        std::lock_guard<std::mutex> objectLock(job.object->mutex());
        job.object->swapPages_nolock();

        // Now currentPage is nextPage. Update its metadata while holding page lock
        // to prevent concurrent reads from seeing torn updates.
        std::lock_guard<std::mutex> pageLock(nextPage->pageMutex);
        nextPage->validAspects |= job.aspects;
        nextPage->generation++;
    }

    // TODO: Phase 5e.6 - emit Qt signal for UI redraw
    // Once SObject is connected to Qt, emit revalidationComplete(object, aspects)
    // For now, UI will re-read via getCapture() on next paint
}

void CaptureRevalidator::dispatchRecomputation(SObject* object, uint32_t aspects, CapturePageData& page) {
    // Phase 5e.5: Dispatch to object's virtual recomputation methods
    //
    // Each SObject type implements its own recomputeXXX methods:
    // - SPlainWave: recomputePreview() uses getStraightPreview()
    // - STrack: recomputePreview() composites from child clips
    // - SStdMixer: recomputePreview() composites from child tracks
    // - SCut: can recomputePreview() from its content
    //
    // The revalidator just dispatches; the actual work is in the object.

    if (aspects & Playback) {
        object->recomputePlayback(page);
    }
    if (aspects & Metadata) {
        object->recomputeMetadata(page);
    }
    if (aspects & Preview) {
        object->recomputePreview(page);
    }
    if (aspects & Export) {
        object->recomputeExport(page);
    }
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

    // Call component->freezePage() to materialize output
    // This internally orchestrates: reset/restore → renderFrames() → capture state
    auto frozenPage = job.component->freezePage(
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
