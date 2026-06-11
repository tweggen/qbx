#include "render_session.h"

#include <chrono>
#include <cmath>
#include <vector>

#include "twcomponent.h"
#include "sapplication.h"

namespace audio {

RenderSession::RenderSession() {}

RenderSession::~RenderSession() {
    if (running_) {
        requestCancel();
        if (renderThread_ && renderThread_->joinable()) {
            renderThread_->join();
        }
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
    // Buffer for audio samples (stereo, 2048 frames)
    const std::size_t RENDER_BUFFER_FRAMES = 2048;
    const std::size_t RENDER_BUFFER_SAMPLES = RENDER_BUFFER_FRAMES * 2;  // Stereo
    std::vector<float> buffer(RENDER_BUFFER_SAMPLES);

    bool success = true;
    std::string errorMsg;

    fprintf(stderr, "[RenderSession] Starting render. Total samples: %zu, Sample rate: %u, Start offset: %zu\n",
            totalSamples_, sampleRate_, startOffsetSamples_);
    fflush(stderr);

    try {
        fprintf(stderr, "\n[RenderSession::DEBUG] ===== RENDER START =====\n");
        fprintf(stderr, "[RenderSession::DEBUG] startOffsetSamples_: %zu\n", startOffsetSamples_);
        fprintf(stderr, "[RenderSession::DEBUG] totalSamples_: %zu\n", totalSamples_);
        fprintf(stderr, "[RenderSession::DEBUG] sampleRate_: %u\n", sampleRate_);
        fprintf(stderr, "[RenderSession::DEBUG] synthOutput_: %p\n", (void*)synthOutput_);
        fprintf(stderr, "[RenderSession::DEBUG] synthOutput_->getNOutputs(): %d\n", synthOutput_->getNOutputs());
        fprintf(stderr, "[RenderSession::DEBUG] synthOutput_->getNInputs(): %d\n", synthOutput_->getNInputs());
        fflush(stderr);

        // Seek to start position (exactly like playback does)
        fprintf(stderr, "[RenderSession::DEBUG] Calling seekTo(%zu)\n", startOffsetSamples_);
        fflush(stderr);
        synthOutput_->seekTo(startOffsetSamples_);
        fprintf(stderr, "[RenderSession::DEBUG] seekTo() returned\n");
        fflush(stderr);

        // Set global locator to start position (exactly like playback does)
        fprintf(stderr, "[RenderSession::DEBUG] Setting global locator to %zu\n", startOffsetSamples_);
        fflush(stderr);
        SApplication::app().setGlobalLocatorPos(startOffsetSamples_);
        fprintf(stderr, "[RenderSession::DEBUG] Global locator set\n");
        fflush(stderr);

        std::size_t samplesGeneratedVal = 0;
        std::size_t samplesWrittenVal = 0;

        fprintf(stderr, "[RenderSession::DEBUG] Will render %zu samples starting from position %zu\n",
                totalSamples_, startOffsetSamples_);
        fflush(stderr);

        while (!cancelRequested_ && samplesWrittenVal < totalSamples_) {
            if (samplesWrittenVal == 0) {
                fprintf(stderr, "[RenderSession::DEBUG] === FIRST ITERATION ===\n");
                fflush(stderr);
            }

            // Calculate how many samples to render this iteration
            std::size_t remainingToRender = totalSamples_ - samplesWrittenVal;
            std::size_t samplesToRender =
                std::min(RENDER_BUFFER_FRAMES, (remainingToRender + 1) / 2);

            fprintf(stderr, "[RenderSession::DEBUG::LOOP] Iteration %zu: samplesWrittenVal=%zu, remainingToRender=%zu, samplesToRender=%zu\n",
                    samplesGeneratedVal / RENDER_BUFFER_FRAMES, samplesWrittenVal, remainingToRender, samplesToRender);
            fflush(stderr);

            // Pull mono samples from synth component (starting from seeked position)
            fprintf(stderr, "[RenderSession::DEBUG::LOOP] Calling calcOutputTo(buffer, %zu, 0)\n", samplesToRender);
            fflush(stderr);
            length_t framesGenerated = synthOutput_->calcOutputTo(
                buffer.data(), samplesToRender, 0);
            fprintf(stderr, "[RenderSession::DEBUG::LOOP] calcOutputTo returned: %lld frames\n", framesGenerated);
            fflush(stderr);

            if (samplesWrittenVal < 100) {  // Debug first iteration
                fprintf(stderr, "[RenderSession::DEBUG::SAMPLES] First few samples: %.6f, %.6f, %.6f, %.6f\n",
                        buffer[0], buffer[1], buffer[2], buffer[3]);
                fflush(stderr);
            }

            // If no frames were generated, fill with silence
            if (framesGenerated <= 0) {
                std::fill(buffer.begin(), buffer.begin() + samplesToRender, 0.0f);
                framesGenerated = samplesToRender;
            }

            // Expand mono to stereo (duplicate each sample for left/right channels)
            for (length_t i = framesGenerated - 1; i >= 0; --i) {
                float s = buffer[i];
                buffer[i * 2] = s;      // Left channel
                buffer[i * 2 + 1] = s;  // Right channel
            }

            // Write all generated frames to file
            if (!writer_->write(buffer.data(), framesGenerated)) {
                success = false;
                errorMsg = std::string("Write failed: ") + writer_->errorMessage();
                break;
            }

            samplesWrittenVal += framesGenerated;
            samplesGeneratedVal += framesGenerated;
            samplesWritten_.store(samplesWrittenVal);

            // Update global locator position (same as playback callback does)
            SApplication::app().setGlobalLocatorPos(startOffsetSamples_ + samplesWrittenVal);

            // Emit progress callback every ~50ms
            if (samplesWrittenVal % (sampleRate_ / 20) == 0) {
                fprintf(stderr, "[RenderSession] Progress: %zu / %zu samples\n",
                        samplesWrittenVal, totalSamples_);
                fflush(stderr);
                if (onProgress) {
                    onProgress(samplesWrittenVal, totalSamples_);
                }
            }
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
