#include "recording_session.h"

#include "audio/audio_input.h"
#include "audio/audio_file_writer.h"

#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <vector>

namespace audio {

RecordingSession::RecordingSession() {}

RecordingSession::~RecordingSession() {
    if (running_) {
        requestStop();
        if (recordThread_ && recordThread_->joinable()) {
            recordThread_->join();
        }
    }
}

bool RecordingSession::start(const RecordingParams &params) {
    if (running_) {
        lastError_ = "Recording already in progress";
        return false;
    }

    if (params.armedTrackIds.empty()) {
        lastError_ = "No tracks armed for recording";
        return false;
    }

    if (params.projectDirectory.empty()) {
        lastError_ = "Project directory not specified";
        return false;
    }

    params_ = params;
    recordedDuration_ = 0.0;
    createdFiles_.clear();

    stopRequested_ = false;
    running_ = true;

    try {
        recordThread_ = std::make_unique<std::thread>([this] { recordThreadMain(); });
    } catch (const std::exception &e) {
        running_ = false;
        lastError_ = std::string("Failed to start recording thread: ") + e.what();
        return false;
    }

    return true;
}

void RecordingSession::requestStop() {
    stopRequested_ = true;
}

bool RecordingSession::isRunning() const {
    return running_;
}

double RecordingSession::recordedDurationSeconds() const {
    return recordedDuration_;
}

const char *RecordingSession::errorMessage() const {
    return lastError_.c_str();
}

const std::vector<std::string> &RecordingSession::createdFiles() const {
    return createdFiles_;
}

void RecordingSession::recordThreadMain() {
    // Create audio input
    std::unique_ptr<AudioInput> input = createAudioInput();
    if (!input) {
        lastError_ = "Failed to create audio input";
        running_ = false;
        if (onComplete) {
            onComplete(false, lastError_.c_str());
        }
        return;
    }

    // Open input device
    if (input->openDevice(params_.inputDeviceId, params_.sampleRate) < 0) {
        lastError_ = std::string("Failed to open input device: ") + input->errorMessage();
        running_ = false;
        if (onComplete) {
            onComplete(false, lastError_.c_str());
        }
        return;
    }

    // Get actual config from input
    const AudioInputConfig &inputConfig = input->getConfig();
    std::uint32_t sampleRate = inputConfig.sampleRate;
    std::uint32_t channels = inputConfig.channels;

    // Create output filename with timestamp
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) %
              1000;

    std::stringstream filenameSS;
    filenameSS << params_.projectDirectory << "/";
    filenameSS << std::put_time(std::localtime(&time), "%Y%m%d_%H%M%S");
    filenameSS << "_" << std::setfill('0') << std::setw(3) << ms.count();
    filenameSS << "_input0.wav";

    std::string filename = filenameSS.str();
    createdFiles_.push_back(filename);

    // Create WAV writer
    std::unique_ptr<AudioFileWriter> writer = createAudioFileWriter(AudioFormat::WAV);
    if (!writer) {
        lastError_ = "Failed to create WAV writer";
        input->closeDevice();
        running_ = false;
        if (onComplete) {
            onComplete(false, lastError_.c_str());
        }
        return;
    }

    // Open WAV file
    AudioFileConfig fileConfig;
    fileConfig.sampleRate = sampleRate;
    fileConfig.channels = channels;
    fileConfig.sampleType = inputConfig.sampleType;

    if (!writer->open(filename, fileConfig)) {
        lastError_ = std::string("Failed to open WAV file: ") + writer->errorMessage();
        input->closeDevice();
        running_ = false;
        if (onComplete) {
            onComplete(false, lastError_.c_str());
        }
        return;
    }

    // Start input capture
    if (input->startCapture() < 0) {
        lastError_ = std::string("Failed to start capture: ") + input->errorMessage();
        writer->close();
        input->closeDevice();
        running_ = false;
        if (onComplete) {
            onComplete(false, lastError_.c_str());
        }
        return;
    }

    // Recording loop
    const std::size_t BUFFER_SIZE = 2048;
    std::vector<float> buffer(BUFFER_SIZE * channels);
    auto lastProgressTime = std::chrono::steady_clock::now();

    bool success = true;
    std::string errorMsg;

    try {
        while (!stopRequested_) {
            // Read from input
            std::int32_t framesRead = input->read(buffer.data(), BUFFER_SIZE);

            if (framesRead < 0) {
                success = false;
                errorMsg = std::string("Read error: ") + input->errorMessage();
                break;
            }

            if (framesRead > 0) {
                // Write to WAV
                if (!writer->write(buffer.data(), framesRead)) {
                    success = false;
                    errorMsg = std::string("Write error: ") + writer->errorMessage();
                    break;
                }

                // Update duration
                recordedDuration_ += static_cast<double>(framesRead) / sampleRate;

                // Emit progress every ~0.1s
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - lastProgressTime);

                if (elapsed.count() >= 100) {
                    if (onProgress) {
                        onProgress(recordedDuration_);
                    }
                    lastProgressTime = now;
                }
            } else {
                // No data available, sleep briefly
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    } catch (const std::exception &e) {
        success = false;
        errorMsg = std::string("Recording exception: ") + e.what();
    }

    // Stop and cleanup
    input->stopCapture();
    input->closeDevice();

    if (!writer->close()) {
        if (success) {  // Only override error if we didn't already have one
            success = false;
            errorMsg = std::string("Failed to close WAV file: ") + writer->errorMessage();
        }
    }

    // Handle cancellation
    if (stopRequested_ && success) {
        // User requested stop - treat as success
    }

    running_ = false;

    // Emit completion callback
    if (onComplete) {
        onComplete(success, errorMsg.c_str());
    }

    lastError_ = errorMsg;
}

}  // namespace audio
