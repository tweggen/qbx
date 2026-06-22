#include "capture_revalidator.h"
#include "capture_page_pool.h"
#include "scut.h"  // Needs access to SCut::swapPages()
#include <cassert>

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

void CaptureRevalidator::scheduleRevalidation(SCut* cut, uint32_t aspects, int priority) {
    if (!cut || aspects == 0) return;

    {
        std::lock_guard<std::mutex> lock(queueLock_);
        jobQueue_.push({cut, aspects, priority});
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
    return jobQueue_.size();
}

void CaptureRevalidator::workerLoop() {
    while (true) {
        CaptureRevalidationJob job{nullptr, 0, 0};

        // Wait for job or shutdown signal
        {
            std::unique_lock<std::mutex> lock(queueLock_);
            queueNotEmpty_.wait(lock, [this]() {
                return !jobQueue_.empty() || shutdown_;
            });

            // If shutting down and queue empty, exit
            if (shutdown_ && jobQueue_.empty()) {
                break;
            }

            // If queue empty (but not shutting down), loop again
            if (jobQueue_.empty()) {
                continue;
            }

            // Dequeue next job (highest priority first)
            job = jobQueue_.top();
            jobQueue_.pop();
        }

        // Process job outside lock (blocking OK for background thread)
        processJob(job);
    }
}

void CaptureRevalidator::processJob(const CaptureRevalidationJob& job) {
    assert(job.cut);
    assert(job.aspects != 0);

    // Check if cut still needs revalidation
    // (state may have changed while job was queued)
    if (!job.cut->needsRevalidation(job.aspects)) {
        return;
    }

    // Allocate nextPage from pool if needed
    auto nextPage = job.cut->getNextPage();
    if (!nextPage) {
        nextPage = pagePool_->allocatePage();
        if (!nextPage) {
            // Pool exhausted: re-queue at lowest priority and skip for now
            scheduleRevalidation(job.cut, job.aspects, 0);
            return;
        }
        job.cut->setNextPage(nextPage);
    }

    // === REVALIDATE (blocking, outside any locks) ===
    // This may take 10-100ms depending on aspect complexity
    if (job.aspects & Playback) {
        recomputePlayback(job.cut, *nextPage);
    }
    if (job.aspects & Metadata) {
        recomputeMetadata(job.cut, *nextPage);
    }
    if (job.aspects & Preview) {
        recomputePreview(job.cut, *nextPage);
    }
    if (job.aspects & Export) {
        recomputeExport(job.cut, *nextPage);
    }

    // === ATOMIC SWAP ===
    // Cut's mutex protects this operation
    {
        std::lock_guard<std::mutex> lock(job.cut->mutex());
        job.cut->swapPages();
        nextPage->validAspects |= job.aspects;
        nextPage->generation++;
    }

    // TODO: Phase 5 - emit Qt signal for UI redraw
    // Once SCut becomes a QObject, emit revalidationComplete(cut, aspects)
    // For now, UI will re-read via getPreviewCapture() on next paint
}

void CaptureRevalidator::recomputePlayback(SCut* cut, CapturePageData& page) {
    // TODO: Rebuild reader chain into page
    // This involves:
    // - Accessing cut's window parameters (startOffset, loopLength, grainParams)
    // - Building twSampleReader chain
    // - Building twGrainSource if needed
    // - Storing reader state in page data
    //
    // For now, mark as valid (placeholder)
    page.validAspects |= Playback;
}

void CaptureRevalidator::recomputeMetadata(SCut* cut, CapturePageData& page) {
    // TODO: Compute duration, peak levels, RMS
    // For now, mark as valid (placeholder)
    page.validAspects |= Metadata;
}

void CaptureRevalidator::recomputePreview(SCut* cut, CapturePageData& page) {
    // TODO: Downsample to waveform peaks
    // For now, mark as valid (placeholder)
    page.validAspects |= Preview;
}

void CaptureRevalidator::recomputeExport(SCut* cut, CapturePageData& page) {
    // TODO: Resample and normalize for export
    // For now, mark as valid (placeholder)
    page.validAspects |= Export;
}
