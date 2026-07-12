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
        std::lock_guard<std::mutex> lock(job.object->revalMutex());

        // Check if object still needs revalidation (state may have changed while queued)
        // _nolock: we hold the lock (std::lock_guard above)
        if (!job.object->revalNeeded_nolock(job.aspects)) {
            return;
        }

        // Get or allocate nextPage (under lock)
        nextPage = job.object->revalGetNextPage_nolock();
        if (!nextPage) {
            nextPage = pagePool_->allocatePage();
            if (!nextPage) {
                // Pool exhausted: re-queue at lowest priority and skip for now
                scheduleRevalidation(job.object, job.aspects, 0);
                return;
            }
            job.object->revalSetNextPage_nolock(nextPage);
        }
    }  // Release lock before potentially long-running recomputation

    // === REVALIDATE (blocking, OUTSIDE locks) ===
    // This may take 10-100ms depending on aspect complexity
    // No locks held, so UI/audio threads can proceed
    // Dispatch to the object's virtual recomputation methods
    dispatchRecomputation(job.object, job.aspects, *nextPage);

    // === CRITICAL SECTION 2: Atomic swap and mark valid ===
    {
        std::lock_guard<std::mutex> objectLock(job.object->revalMutex());
        job.object->revalSwapPages_nolock();

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

void CaptureRevalidator::dispatchRecomputation(IRevalidatable* object, uint32_t aspects, CapturePageData& page) {
    // Phase 5: Unified rendering dispatch
    // Preview now uses freezePreviewPage() pipeline (same as playback);
    // Metadata/Export still use legacy recomputeXXX for now.

    if (aspects & Preview) {
        // Phase 5: Use freezePreviewPage() for preview rendering
        // Provides lower-resolution waveform data via unified DSP pipeline.
        // Falls back to previous page if new page not ready (non-blocking).

        twComponent &rootComp = object->revalRootComponent();

        // Preview rendering parameters:
        // - previewSampleRate: 1000 Hz typical for waveform visualization
        // - fullSampleRate: actual component rate for state consistency
        // - previousPage: current page for fallback if new page not ready
        int previewRate = 1000;  // Configurable: lower = coarser visualization
        int fullRate = 48000;    // TODO: get from environment

        auto frozenPage = rootComp.freezePreviewPage(
            0,                       // startPos: 0 for full preview
            page.PAGE_SIZE / sizeof(float),  // length: full page capacity
            previewRate,
            fullRate,
            nullptr                  // No previous page (revalidator manages fallback)
        );

        if( frozenPage && frozenPage->validFrames > 0 ) {
            // Copy frozen preview samples into CapturePageData
            size_t samplesToCopy = std::min((size_t)frozenPage->validFrames, (size_t)page.PAGE_SIZE / sizeof(float));
            if( samplesToCopy > 0 ) {
                memcpy(page.data, frozenPage->samples.data(), samplesToCopy * sizeof(float));
                page.validAspects |= Preview;
            }
        }
    }

    if (aspects & Metadata) {
        object->revalRecomputeMetadata(page);
    }
    if (aspects & Export) {
        object->revalRecomputeExport(page);
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
