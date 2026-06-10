#include "render_session.h"

#include <chrono>
#include <cmath>
#include <vector>

#include "twcomponent.h"

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

    try {
        while (!cancelRequested_ && samplesWritten_ < totalSamples_) {
            // Calculate how many samples to render this iteration
            std::size_t remainingSamples = totalSamples_ - samplesWritten_;
            std::size_t samplesToRender =
                std::min(RENDER_BUFFER_FRAMES, (remainingSamples + 1) / 2);

            // Pull samples from synth
            // Note: This is a simplified implementation. The actual call depends on
            // how twComponent's output is accessed. For now, we zero-fill as a
            // placeholder.
            // TODO: Properly integrate with synth graph audio processing
            std::fill(buffer.begin(), buffer.end(), 0.0f);

            // Write to file
            if (!writer_->write(buffer.data(), samplesToRender)) {
                success = false;
                errorMsg = std::string("Write failed: ") + writer_->errorMessage();
                break;
            }

            samplesWritten_ += samplesToRender;

            // Emit progress callback every ~50ms
            if (samplesWritten_ % (sampleRate_ / 20) == 0) {
                if (onProgress) {
                    onProgress(samplesWritten_, totalSamples_);
                }
            }
        }

        if (cancelRequested_) {
            success = false;
            errorMsg = "Render cancelled";
        }

    } catch (const std::exception &e) {
        success = false;
        errorMsg = std::string("Render error: ") + e.what();
    }

    // Close file and clean up
    if (writer_) {
        writer_->close();
    }

    running_ = false;

    // Emit completion callback
    if (onComplete) {
        onComplete(success, errorMsg.c_str());
    }

    lastError_ = errorMsg;
}

}  // namespace audio
