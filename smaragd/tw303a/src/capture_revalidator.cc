#include "capture_revalidator.h"
#include "capture_page_pool.h"
#include "scut.h"  // Needs access to SCut::swapPages()
#include "twcomponent.h"
#include "tw303aenv.h"
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

    std::shared_ptr<CapturePageData> nextPage;

    // === CRITICAL SECTION 1: Check state and allocate page ===
    {
        std::lock_guard<std::mutex> lock(job.cut->mutex());

        // Check if cut still needs revalidation
        // (state may have changed while job was queued)
        if (!job.cut->needsRevalidation(job.aspects)) {
            return;
        }

        // Get or allocate nextPage (under lock)
        nextPage = job.cut->getNextPage_nolock();
        if (!nextPage) {
            nextPage = pagePool_->allocatePage();
            if (!nextPage) {
                // Pool exhausted: re-queue at lowest priority and skip for now
                scheduleRevalidation(job.cut, job.aspects, 0);
                return;
            }
            job.cut->setNextPage_nolock(nextPage);
        }
    }  // Release lock before potentially long-running recomputation

    // === REVALIDATE (blocking, OUTSIDE locks) ===
    // This may take 10-100ms depending on aspect complexity
    // No locks held, so UI/audio threads can proceed
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

    // === CRITICAL SECTION 2: Atomic swap ===
    {
        std::lock_guard<std::mutex> lock(job.cut->mutex());
        job.cut->swapPages_nolock();
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
    // Phase 5d: Build preview waveform from cut's content
    //
    // Preview is stored as an array of preview_t structs (min/max envelopes).
    // Maximum number of preview samples: PAGE_SIZE / sizeof(preview_t)
    // = 256KB / 2 bytes = 131,072 preview points

    const int MAX_PREVIEW_SAMPLES = CapturePageData::PAGE_SIZE / sizeof(preview_t);
    preview_t* previewData = reinterpret_cast<preview_t*>(page.data);

    // Get cut's content
    SObject& content = cut->getContent();

    if (content.getRandomSource()) {
        // Sample-backed cut: use content's getPreview() to get waveform
        // This works because SPlainWave implements getPreview()
        int result = content.getPreview(
            previewData,
            cut->getStartOffset(),
            cut->getDuration(),
            MAX_PREVIEW_SAMPLES
        );

        if (result >= 0) {
            page.validAspects |= Preview;
        }
    } else {
        // Container-backed cut (track, group, etc.)
        // Render the component graph to audio, then downsample to preview peaks

        // Initialize preview data to silence
        for (int i = 0; i < MAX_PREVIEW_SAMPLES; ++i) {
            previewData[i].min = 0;
            previewData[i].max = 0;
        }

        const length_t TOTAL_RENDER_SAMPLES = cut->getDuration();
        if (TOTAL_RENDER_SAMPLES == 0) {
            // Zero-duration cut: already filled with silence above
            page.validAspects |= Preview;
            return;
        }

        try {
            twComponent& component = content.getRootComponent();

            // Render in chunks and downsample to preview
            // Use a reasonable chunk size (4096 samples = ~85ms @ 48kHz)
            const length_t RENDER_CHUNK_SIZE = 4096;

            std::vector<sample_t> audioBuffer(RENDER_CHUNK_SIZE);
            int previewIndex = 0;

            for (length_t renderPos = 0; renderPos < TOTAL_RENDER_SAMPLES && previewIndex < MAX_PREVIEW_SAMPLES;
                 renderPos += RENDER_CHUNK_SIZE) {

                length_t chunkSize = std::min(RENDER_CHUNK_SIZE, TOTAL_RENDER_SAMPLES - renderPos);

                // Render stereo: average both channels for preview
                for (int ch = 0; ch < 2; ++ch) {
                    try {
                        length_t rendered = component.calcOutputTo(audioBuffer.data(), chunkSize, ch);
                        if (rendered == 0) break;

                        // Downsample chunk to single preview point per channel
                        sample_t minVal = audioBuffer[0];
                        sample_t maxVal = audioBuffer[0];

                        for (length_t i = 0; i < rendered; ++i) {
                            minVal = std::min(minVal, audioBuffer[i]);
                            maxVal = std::max(maxVal, audioBuffer[i]);
                        }

                        // Accumulate across channels
                        if (ch == 0) {
                            previewData[previewIndex].min = static_cast<signed char>(minVal * 127.0f);
                            previewData[previewIndex].max = static_cast<signed char>(maxVal * 127.0f);
                        } else {
                            // Average with second channel
                            signed char minCh = static_cast<signed char>(minVal * 127.0f);
                            signed char maxCh = static_cast<signed char>(maxVal * 127.0f);
                            previewData[previewIndex].min = (previewData[previewIndex].min + minCh) / 2;
                            previewData[previewIndex].max = (previewData[previewIndex].max + maxCh) / 2;
                        }
                    } catch (...) {
                        // Component rendering failed; use silence
                        previewData[previewIndex].min = 0;
                        previewData[previewIndex].max = 0;
                        break;
                    }
                }

                previewIndex++;
            }

            page.validAspects |= Preview;
        } catch (...) {
            // Container rendering failed; preview already filled with silence
            page.validAspects |= Preview;
        }
    }
}

void CaptureRevalidator::recomputeExport(SCut* cut, CapturePageData& page) {
    // TODO: Resample and normalize for export
    // For now, mark as valid (placeholder)
    page.validAspects |= Export;
}
