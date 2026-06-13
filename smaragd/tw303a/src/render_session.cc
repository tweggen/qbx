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
        fprintf(stderr, "\n[RenderSession::DEBUG] ===== RENDER DIAGNOSTICS START =====\n");
        fprintf(stderr, "[RenderSession::DEBUG] Input parameters:\n");
        fprintf(stderr, "[RenderSession::DEBUG]   startTimeSec: %.6f seconds\n", params_.startTimeSec);
        fprintf(stderr, "[RenderSession::DEBUG]   endTimeSec: %.6f seconds\n", params_.endTimeSec);
        fprintf(stderr, "[RenderSession::DEBUG]   sampleRate: %u Hz\n", sampleRate_);
        fprintf(stderr, "[RenderSession::DEBUG]   startOffsetSamples_: %zu samples\n", startOffsetSamples_);
        fprintf(stderr, "[RenderSession::DEBUG]   totalSamples_: %zu samples\n", totalSamples_);
        fflush(stderr);

        // Seek to start position (exactly like playback does)
        fprintf(stderr, "[RenderSession::DEBUG] SEEKING synth to position %zu\n", startOffsetSamples_);
        fflush(stderr);
        synthOutput_->seekTo(startOffsetSamples_);
        fprintf(stderr, "[RenderSession::DEBUG] seekTo() completed\n");
        fflush(stderr);

        // Set global locator to start position (exactly like playback does)
        fprintf(stderr, "[RenderSession::DEBUG] Setting global locator to %zu\n", startOffsetSamples_);
        fflush(stderr);
        SApplication::app().setGlobalLocatorPos(startOffsetSamples_);
        fprintf(stderr, "[RenderSession::DEBUG] Global locator set\n");
        fflush(stderr);

        // Configure resampler with the same rate for input and output (no resampling needed)
        fprintf(stderr, "[RenderSession::DEBUG] Configuring resampler: inRate=%u, outRate=%u\n", sampleRate_, sampleRate_);
        fflush(stderr);
        resampler_.configure(sampleRate_, sampleRate_);
        resampler_.reserveHint((length_t) RENDER_BUFFER_FRAMES);
        resampler_.reset();
        fprintf(stderr, "[RenderSession::DEBUG] Resampler configured and reset\n");
        fflush(stderr);

        std::size_t samplesWrittenVal = 0;

        fprintf(stderr, "[RenderSession::DEBUG] Will render %zu samples starting from position %zu\n",
                totalSamples_, startOffsetSamples_);
        fflush(stderr);

        // Get synth's output plug (same wiring as playback)
        fprintf(stderr, "[RenderSession::DEBUG] Getting synth output plug via linkOutput(0)\n");
        fflush(stderr);
        twLatchOutput *synthOutputPlug = synthOutput_->linkOutput(0);
        fprintf(stderr, "[RenderSession::DEBUG] linkOutput(0) returned: %p\n", (void*)synthOutputPlug);
        fflush(stderr);

        if (!synthOutputPlug) {
            errorMsg = "Failed to get synth output plug";
            success = false;
        } else {
            fprintf(stderr, "[RenderSession::DEBUG] Entering render loop\n");
            fflush(stderr);

            int iterationCount = 0;
            while (!cancelRequested_ && samplesWrittenVal < totalSamples_) {
                // Calculate how many frames to render this iteration
                std::size_t remainingToRender = totalSamples_ - samplesWrittenVal;
                std::size_t framesToRender = std::min(RENDER_BUFFER_FRAMES, remainingToRender);

                if (iterationCount == 0) {
                    fprintf(stderr, "[RenderSession::DEBUG] FIRST ITERATION:\n");
                    fprintf(stderr, "[RenderSession::DEBUG]   framesToRender: %zu\n", framesToRender);
                    fprintf(stderr, "[RenderSession::DEBUG]   samplesWrittenVal: %zu\n", samplesWrittenVal);
                    fflush(stderr);
                }

                // Pull mono samples from synth via resampler (same interface as playback)
                length_t inConsumed = 0;
                length_t framesGenerated = resampler_.process(
                    static_cast<twLatchStreamingOutput *>(synthOutputPlug),
                    buffer.data(), (length_t) framesToRender, &inConsumed);

                if (iterationCount == 0) {
                    fprintf(stderr, "[RenderSession::DEBUG]   framesGenerated: %lld\n", framesGenerated);
                    fprintf(stderr, "[RenderSession::DEBUG]   inConsumed: %lld\n", inConsumed);
                    fprintf(stderr, "[RenderSession::DEBUG]   First 8 samples: %.0f, %.0f, %.0f, %.0f, %.0f, %.0f, %.0f, %.0f\n",
                            buffer[0], buffer[1], buffer[2], buffer[3], buffer[4], buffer[5], buffer[6], buffer[7]);
                    fflush(stderr);
                }

                if (framesGenerated <= 0) {
                    std::fill(buffer.begin(), buffer.begin() + framesToRender, 0.0f);
                    framesGenerated = framesToRender;
                }

                // Expand mono to stereo (duplicate each sample for left/right channels)
                // Same logic as playback callback does
                for (length_t i = framesGenerated - 1; i >= 0; --i) {
                    float s = buffer[i];
                    buffer[i * 2] = s;      // Left channel
                    buffer[i * 2 + 1] = s;  // Right channel
                }

                framesToRender = framesGenerated;

                // Write all generated frames to file
                if (!writer_->write(buffer.data(), framesToRender)) {
                    success = false;
                    errorMsg = std::string("Write failed: ") + writer_->errorMessage();
                    break;
                }

                samplesWrittenVal += framesToRender;
                samplesWritten_.store(samplesWrittenVal);

                // Update global locator position (same as playback callback does)
                offset_t formerPos = SApplication::app().getGlobalLocatorPos();
                SApplication::app().setGlobalLocatorPos(formerPos + framesToRender);

                // Emit progress callback every ~50ms
                if (samplesWrittenVal % (sampleRate_ / 20) == 0) {
                    fprintf(stderr, "[RenderSession] Progress: %zu / %zu samples\n",
                            samplesWrittenVal, totalSamples_);
                    fflush(stderr);
                    if (onProgress) {
                        onProgress(samplesWrittenVal, totalSamples_);
                    }
                }
                iterationCount++;
            }
            fprintf(stderr, "[RenderSession::DEBUG] Exited render loop after %d iterations\n", iterationCount);
            fflush(stderr);
        }

        fprintf(stderr, "[RenderSession] Render loop complete. Total samples written: %zu\n",
                samplesWrittenVal);
        fflush(stderr);

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
