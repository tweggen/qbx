#include "tw/render/render_session.h"

#include <chrono>
#include <cmath>
#include <vector>

#include "tw/graph/twcomponent.h"
#include "tw/schedule/capture_revalidator.h"   // Stage 4: watermark demands
#include "tw/sources/twresampler.h"
#include "tw/core/twlog.h"

namespace audio {

RenderSession::RenderSession() {}

RenderSession::~RenderSession() {
    // CRITICAL: Always join the thread if it exists, regardless of running_ state.
    // running_ is set to false at the END of renderThreadMain(), so it can be false
    // while the thread is still technically alive and joinable. Failing to join
    // before destroying the std::thread object causes a crash/abort.
    if (renderThread_ && renderThread_->joinable()) {
        requestCancel();
        renderThread_->join();
    }
}

bool RenderSession::start(std::shared_ptr<twComponent> synthOutput, const RenderParams &params,
                           std::uint32_t sampleRate) {
    if (running_) {
        lastError_ = "Render already in progress";
        return false;
    }

    // Ensure previous render thread is fully cleaned up before starting a new one
    if (renderThread_ && renderThread_->joinable()) {
        renderThread_->join();
        renderThread_.reset();
    }

    if (!synthOutput) {
        lastError_ = "No synth output component";
        return false;
    }

    if (params.outputPath.empty()) {
        lastError_ = "Output path not specified";
        return false;
    }

    if (params.endTimeSec <= params.startTimeSec) {
        lastError_ = "Invalid time range";
        return false;
    }

    synthOutput_ = synthOutput;
    params_ = params;
    sampleRate_ = sampleRate;

    // Calculate total samples to render
    double durationSec = params_.endTimeSec - params_.startTimeSec;
    totalSamples_ = static_cast<std::size_t>(durationSec * sampleRate_);

    // Calculate start offset in samples
    startOffsetSamples_ = static_cast<std::size_t>(params_.startTimeSec * sampleRate_);

    // Create writer for the selected format
    writer_ = createAudioFileWriter(params_.format);
    if (!writer_) {
        lastError_ = "Unsupported audio format";
        return false;
    }

    // Open file
    AudioFileConfig config;
    config.sampleRate = sampleRate_;
    config.channels = 2;  // Stereo output
    config.sampleType = twSampleType::Float32;

    if (!writer_->open(params_.outputPath, config)) {
        lastError_ = std::string("Failed to open output file: ") + writer_->errorMessage();
        writer_.reset();
        return false;
    }

    // Start render thread
    samplesWritten_ = 0;
    cancelRequested_ = false;
    running_ = true;

    try {
        renderThread_ = std::make_unique<std::thread>([this] { renderThreadMain(); });
    } catch (const std::exception &e) {
        running_ = false;
        lastError_ = std::string("Failed to start render thread: ") + e.what();
        writer_->close();
        writer_.reset();
        return false;
    }

    return true;
}

void RenderSession::requestCancel() {
    cancelRequested_ = true;
}

bool RenderSession::isRunning() const {
    return running_;
}

std::size_t RenderSession::samplesWritten() const {
    return samplesWritten_;
}

std::size_t RenderSession::totalSamples() const {
    return totalSamples_;
}

const char *RenderSession::errorMessage() const {
    return lastError_.c_str();
}

void RenderSession::renderThreadMain() {
    const std::size_t RENDER_BUFFER_FRAMES = 2048;
    const std::size_t RENDER_BUFFER_SAMPLES = RENDER_BUFFER_FRAMES * 2;  // Stereo
    std::vector<float> buffer(RENDER_BUFFER_SAMPLES);

    bool success = true;
    std::string errorMsg;

    TW_LOGI( "render", "[RenderSession] Starting render. Total samples: %zu, Sample rate: %u, Start offset: %zu",
            totalSamples_, sampleRate_, startOffsetSamples_ );

    try {
        TW_LOGI( "render", "[RenderSession] Starting unified render via AudioEngine + FileSink" );
        TW_LOGD( "render", "[RenderSession] Range: %.2f - %.2f seconds (%zu samples)",
                params_.startTimeSec, params_.endTimeSec, totalSamples_ );

        // Phase 3: Sequential rendering via freezePage() (no seekTo)
        // Strategy: Use freezePage() to render sequentially, eliminating seekTo() state corruption
        // freezePage() manages state reset/restore internally, enabling correct sequential DSP

        // Reset component to start of render range
        synthOutput_->reset();
        if (onPosition) onPosition(startOffsetSamples_);

        // Create FileSink for buffered file output with futures-based waiting
        fileSink_ = std::make_unique<FileSink>(writer_.get());

        std::size_t samplesWrittenVal = 0;

        // Render loop: sequentially freeze pages from synth output
        // freezePage() orchestrates reset/restore/render/capture without seekTo()
        const std::size_t BLOCK_SIZE = 2048;
        // Phase 5 Gap 11: Use unified page size constant
        const uint64_t PAGE_SIZE = twOutputPage::PAGE_SIZE / sizeof(sample_t);  // Pages from unified constant

        std::vector<float> bufL(BLOCK_SIZE), bufR(BLOCK_SIZE);
        std::shared_ptr<twOutputPage> prevPage;

        // Proposal 19 dataflow stage 4: as a watermark consumer the render
        // demands pages ONE AT A TIME, strictly sequentially (the per-page
        // wait in the loop below). Within each page the scheduler still
        // parallelizes across the graph (a page's trackmix/reader/chain nodes
        // run concurrently on the pool), but pages of one component execute in
        // position order — the legacy cadence.
        //
        // Deliberately NO full-range look-ahead demand: non-caching components
        // (twTrackMix mints a fresh page every freeze) would be re-rendered by
        // a later per-page demand OUT OF POSITION ORDER against the full
        // demand's in-order chain, racing their internal per-clip state
        // (clip.previousPage) — observed as a nondeterministically missing
        // track contribution in the goldens. Cross-page pipelining can return
        // once node results are the cache for non-caching components too.
        uint64_t awaitedPage = (uint64_t)-1;

        while (!cancelRequested_ && samplesWrittenVal < totalSamples_) {
            // Current position in component graph samples. The render range may
            // start mid-project (marked in/out range), so the graph position is
            // the range start plus what we've written so far — not just the
            // written count (that rendered the wrong region for ranges > 0).
            uint64_t currentPos = startOffsetSamples_ + samplesWrittenVal;
            offset_t pageStartPos = (currentPos / PAGE_SIZE) * PAGE_SIZE;

            // Stage 4: wait for the scheduler to have this page frozen — a
            // single-page demand that dedups onto the in-flight full-range
            // node. This is the pipeline's ONLY blocking point, at the edge:
            // the render observes pages, it no longer executes the graph.
            // The requestPage below is then a cache hit; if an edit landed in
            // between (or the demand was aborted by shutdown), it re-renders
            // synchronously — the unchanged legacy fallback.
            if (scheduler_ && pageStartPos != awaitedPage) {
                scheduler_->requestGraphPages(synthOutput_, pageStartPos, 1,
                                              /*priority*/ 8)->wait();
                awaitedPage = pageStartPos;
            }

            // Freeze this page sequentially (via requestPage, Phase 2a: dedups
            // against any concurrent revalidation of the same page).
            // prevPage is used for state resumption (page N resumes from page N-1)
            auto frozenPage = synthOutput_->requestPage(
                pageStartPos,
                nullptr,            // No pre-prepared input; renderFrames uses latches
                0,
                0,
                sampleRate_,
                prevPage            // Sequential state chain: page N resumes from page N-1
            );

            if (!frozenPage || frozenPage->validFrames == 0) {
                break;  // No more audio
            }

            // How many frames to extract from this frozen page
            std::size_t pageOffset = (currentPos % PAGE_SIZE);
            std::size_t framesAvailable = frozenPage->validFrames - pageOffset;
            std::size_t toRender = std::min({
                BLOCK_SIZE,
                totalSamples_ - samplesWrittenVal,
                framesAvailable
            });

            if (toRender == 0) {
                prevPage = frozenPage;
                break;
            }

            // Extract L/R samples from frozen page
            // Page stores all channels interleaved or per-output
            // For now, treat as mono per output; scale to stereo by duplicating
            const sample_t *pageData = frozenPage->samples.data();
            for (std::size_t i = 0; i < toRender; ++i) {
                float sample = pageData[pageOffset + i];
                bufL[i] = sample;
                bufR[i] = sample;  // Duplicate to stereo (temporary; proper multi-channel TBD)
            }

            // Write to sink
            for (std::size_t i = 0; i < toRender; ++i) {
                AudioFrame frame(bufL[i], bufR[i], sampleRate_);
                fileSink_->writeFrame(frame);
                samplesWrittenVal++;
            }

            samplesWritten_.store(samplesWrittenVal);
            if (onPosition) onPosition(startOffsetSamples_ + samplesWrittenVal);

            // Progress every ~50ms
            if (samplesWrittenVal % std::max(1u, sampleRate_ / 20) == 0) {
                TW_LOGD( "render", "[RenderSession] Progress: %zu / %zu samples (sequential pages, no seekTo)",
                        samplesWrittenVal, totalSamples_ );
                if (onProgress) {
                    onProgress(samplesWrittenVal, totalSamples_);
                }
            }

            prevPage = frozenPage;
        }

        TW_LOGD( "render", "[RenderSession] Render loop complete. Frames: %zu", samplesWrittenVal );

        // Flush any buffered frames (FileSink handles futures-based waiting)
        if (fileSink_) {
            fileSink_->flush();
        }

        if (cancelRequested_) {
            success = false;
            errorMsg = "Render cancelled";
        }

    } catch (const std::exception &e) {
        success = false;
        errorMsg = std::string("Render error: ") + e.what();
        TW_LOGD( "render", "[RenderSession] Exception: %s", errorMsg.c_str() );
    }

    // Close file and clean up
    if (writer_) {
        writer_->close();
    }

    TW_LOGD( "render", "[RenderSession] Render complete. Success: %s",
            success ? "true" : "false" );

    // Emit completion callback BEFORE clearing running_: a consumer that polls
    // isRunning() (SRenderAction) must not observe completion until onComplete
    // has run, or it could start the next render while this one's teardown
    // (e.g. resuming background revalidation) is still pending. (Proposal 19
    // Phase 2b: the render-quiesce resume lives in onComplete.)
    if (onComplete) {
        TW_LOGD( "render", "[RenderSession] Calling onComplete callback" );
        onComplete(success, errorMsg.c_str());
    }

    lastError_ = errorMsg;
    running_ = false;
}

}  // namespace audio
