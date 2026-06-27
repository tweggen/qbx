#include "render_session.h"

#include <chrono>
#include <cmath>
#include <vector>

#include "twcomponent.h"
#include "twresampler.h"
#include "sapplication.h"

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

bool RenderSession::start(twComponent *synthOutput, const RenderParams &params,
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

    fprintf(stderr, "[RenderSession] Starting render. Total samples: %zu, Sample rate: %u, Start offset: %zu\n",
            totalSamples_, sampleRate_, startOffsetSamples_);
    fflush(stderr);

    try {
        fprintf(stderr, "[RenderSession] Starting unified render via AudioEngine + FileSink\n");
        fprintf(stderr, "[RenderSession] Range: %.2f - %.2f seconds (%zu samples)\n",
                params_.startTimeSec, params_.endTimeSec, totalSamples_);
        fflush(stderr);

        // Phase 3: Sequential rendering via freezePage() (no seekTo)
        // Strategy: Use freezePage() to render sequentially, eliminating seekTo() state corruption
        // freezePage() manages state reset/restore internally, enabling correct sequential DSP

        // Reset component to start of render range
        synthOutput_->reset();
        SApplication::app().setGlobalLocatorPosRealtime(startOffsetSamples_);

        // Create FileSink for buffered file output with futures-based waiting
        fileSink_ = std::make_unique<FileSink>(writer_.get());

        std::size_t samplesWrittenVal = 0;

        // Render loop: sequentially freeze pages from synth output
        // freezePage() orchestrates reset/restore/render/capture without seekTo()
        const std::size_t BLOCK_SIZE = 2048;
        const uint64_t PAGE_SIZE = 256 * 1024 / sizeof(sample_t);  // ~256kB per page in samples

        std::vector<float> bufL(BLOCK_SIZE), bufR(BLOCK_SIZE);
        std::shared_ptr<twOutputPage> prevPage;

        while (!cancelRequested_ && samplesWrittenVal < totalSamples_) {
            // Current position in component graph samples
            uint64_t currentPos = samplesWrittenVal;
            uint64_t pageStartPos = (currentPos / PAGE_SIZE) * PAGE_SIZE;

            // Freeze this page sequentially
            // prevPage is used for state resumption (page N resumes from page N-1)
            auto frozenPage = synthOutput_->freezePage(
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
            SApplication::app().setGlobalLocatorPosRealtime(startOffsetSamples_ + samplesWrittenVal);

            // Progress every ~50ms
            if (samplesWrittenVal % std::max(1u, sampleRate_ / 20) == 0) {
                fprintf(stderr, "[RenderSession] Progress: %zu / %zu samples (sequential pages, no seekTo)\n",
                        samplesWrittenVal, totalSamples_);
                fflush(stderr);
                if (onProgress) {
                    onProgress(samplesWrittenVal, totalSamples_);
                }
            }

            prevPage = frozenPage;
        }

        fprintf(stderr, "[RenderSession] Render loop complete. Frames: %zu\n", samplesWrittenVal);
        fflush(stderr);

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
        fprintf(stderr, "[RenderSession] Exception: %s\n", errorMsg.c_str());
        fflush(stderr);
    }

    // Close file and clean up
    if (writer_) {
        writer_->close();
    }

    running_ = false;

    fprintf(stderr, "[RenderSession] Render complete. Success: %s\n",
            success ? "true" : "false");
    fflush(stderr);

    // Emit completion callback
    if (onComplete) {
        fprintf(stderr, "[RenderSession] Calling onComplete callback\n");
        fflush(stderr);
        onComplete(success, errorMsg.c_str());
    }

    lastError_ = errorMsg;
}

}  // namespace audio
