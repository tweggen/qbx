#include "recording_session.h"

#include "audio/audio_input.h"
#include "audio/audio_file_writer.h"
#include "sapplication.h"

#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <vector>

namespace audio {

namespace {

// Stateful streaming linear resampler for interleaved float frames. Shared-mode
// WASAPI capture is locked to the device's mix rate, so we capture at that rate
// and convert here to the project rate before writing the WAV (so recorded files
// match the project rate). Linear quality matches the engine's twResampler bar;
// that converter pulls from a streaming-latch component and doesn't fit this
// push-buffer path, so this is a small standalone version. Passthrough when the
// rates already match.
class LinearResampler {
public:
    LinearResampler(std::uint32_t inRate, std::uint32_t outRate, std::uint32_t channels)
        : ratio_(static_cast<double>(inRate) / static_cast<double>(outRate)),
          channels_(channels),
          passthrough_(inRate == outRate),
          havePrev_(false),
          frac_(0.0),
          prev_(channels, 0.0f) {}

    bool passthrough() const { return passthrough_; }

    // Resample `inFrames` interleaved input frames from `in` into `out` (cleared
    // and filled with interleaved output frames). Returns the output frame count.
    std::size_t process(const float *in, std::size_t inFrames,
                        std::vector<float> &out) {
        out.clear();
        if (inFrames == 0) return 0;
        // Upper bound on outputs this chunk; reserve to avoid reallocation churn.
        out.reserve(static_cast<std::size_t>(inFrames / ratio_ + 2.0) * channels_);
        for (std::size_t f = 0; f < inFrames; ++f) {
            const float *cur = in + f * channels_;
            if (!havePrev_) {
                for (std::uint32_t c = 0; c < channels_; ++c) prev_[c] = cur[c];
                havePrev_ = true;
                continue;  // need a previous frame before interpolating an interval
            }
            // Emit every output sample that falls in the interval [prev_, cur).
            while (frac_ < 1.0) {
                for (std::uint32_t c = 0; c < channels_; ++c) {
                    out.push_back(static_cast<float>(
                        prev_[c] * (1.0 - frac_) + cur[c] * frac_));
                }
                frac_ += ratio_;
            }
            frac_ -= 1.0;
            for (std::uint32_t c = 0; c < channels_; ++c) prev_[c] = cur[c];
        }
        return out.size() / channels_;
    }

private:
    double ratio_;            // input frames advanced per output frame
    std::uint32_t channels_;
    bool passthrough_;
    bool havePrev_;
    double frac_;             // position of next output within [prev_, cur), in [0,1)
    std::vector<float> prev_; // last input frame consumed
};

}  // namespace

RecordingSession::RecordingSession() {}

RecordingSession::~RecordingSession() {
    requestStop();
    // Always join a joinable thread, even after normal completion: recordThreadMain
    // clears running_ before it returns, so gating the join on running_ left the
    // finished thread un-joined (it would lurk, and a later destroy/restart hit
    // std::thread's terminate-on-joinable).
    if (recordThread_ && recordThread_->joinable()) {
        recordThread_->join();
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

    // Join a previous (already finished) thread before reusing the handle —
    // assigning over a joinable std::thread would call std::terminate.
    if (recordThread_ && recordThread_->joinable()) {
        recordThread_->join();
    }

    params_ = params;
    recordedDuration_ = 0.0;
    createdFiles_.clear();

    stopRequested_ = false;
    finished_ = false;
    succeeded_ = false;
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

bool RecordingSession::isFinished() const {
    return finished_.load(std::memory_order_acquire);
}

bool RecordingSession::succeeded() const {
    return succeeded_.load();
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
    // Publish the terminal state for the polling UI. lastError_ must already be
    // set; finished_ is stored last with release ordering so a GUI thread that
    // sees isFinished()==true also sees succeeded_/lastError_.
    auto markFinished = [this](bool ok) {
        succeeded_.store(ok);
        running_.store(false);
        finished_.store(true, std::memory_order_release);
    };

    // Create audio input
    std::unique_ptr<AudioInput> input = createAudioInput();
    if (!input) {
        lastError_ = "Failed to create audio input";
        markFinished(false);
        if (onComplete) {
            onComplete(false, lastError_.c_str());
        }
        return;
    }

    // Open input device
    if (input->openDevice(params_.inputDeviceId, params_.sampleRate) < 0) {
        lastError_ = std::string("Failed to open input device: ") + input->errorMessage();
        markFinished(false);
        if (onComplete) {
            onComplete(false, lastError_.c_str());
        }
        return;
    }

    // Get actual config from input. Shared-mode capture runs at the DEVICE rate;
    // we resample to the project (target) rate so the recorded WAV matches the
    // project, per the recording design (files match project rate).
    const AudioInputConfig &inputConfig = input->getConfig();
    std::uint32_t deviceRate = inputConfig.sampleRate;
    std::uint32_t targetRate = params_.sampleRate > 0 ? params_.sampleRate : deviceRate;
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
        markFinished(false);
        if (onComplete) {
            onComplete(false, lastError_.c_str());
        }
        return;
    }

    // Open WAV file
    AudioFileConfig fileConfig;
    fileConfig.sampleRate = targetRate;
    fileConfig.channels = channels;
    fileConfig.sampleType = inputConfig.sampleType;

    if (!writer->open(filename, fileConfig)) {
        lastError_ = std::string("Failed to open WAV file: ") + writer->errorMessage();
        input->closeDevice();
        markFinished(false);
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
        markFinished(false);
        if (onComplete) {
            onComplete(false, lastError_.c_str());
        }
        return;
    }

    // Recording loop
    const std::size_t BUFFER_SIZE = 2048;
    std::vector<float> buffer(BUFFER_SIZE * channels);
    std::vector<float> resampled;  // scratch for device->project rate conversion
    LinearResampler resampler(deviceRate, targetRate, channels);
    auto lastProgressTime = std::chrono::steady_clock::now();

    bool success = true;
    std::string errorMsg;

    // Advance the global playback locator in lock-step with what we capture, so
    // the playhead tracks the recording. Start from the locator position the user
    // armed at; advance by output (project-rate) frames written. Realtime store
    // only — never emit Qt signals from this worker thread (a main-thread QTimer
    // turns these stores into playhead repaints; see SApplication::pumpLocator).
    const offset_t startLocator = SApplication::app().getGlobalLocatorPos();
    offset_t recordedProjectFrames = 0;

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
                // Resample device-rate capture to the project rate, then write.
                const float *outData = buffer.data();
                std::int32_t outFrames = framesRead;
                if (!resampler.passthrough()) {
                    std::size_t n = resampler.process(buffer.data(),
                                                      static_cast<std::size_t>(framesRead),
                                                      resampled);
                    outData = resampled.data();
                    outFrames = static_cast<std::int32_t>(n);
                }

                // Write to WAV (a chunk can resample to 0 output frames — skip it)
                if (outFrames > 0 && !writer->write(outData, outFrames)) {
                    success = false;
                    errorMsg = std::string("Write error: ") + writer->errorMessage();
                    break;
                }

                // Update duration from the input frames at the device rate (real
                // elapsed time, independent of the resampling ratio).
                recordedDuration_.store(
                    recordedDuration_.load() + static_cast<double>(framesRead) / deviceRate
                );

                // Advance the playhead by the project-rate frames just captured.
                if (outFrames > 0) {
                    recordedProjectFrames += static_cast<offset_t>(outFrames);
                    SApplication::app().setGlobalLocatorPosRealtime(
                        startLocator + recordedProjectFrames);
                }

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

    // Publish error/result before marking finished (release), so the polling UI
    // sees a consistent terminal state.
    lastError_ = errorMsg;
    markFinished(success);

    // Legacy callback (not used by the progress dialog, which polls instead).
    if (onComplete) {
        onComplete(success, errorMsg.c_str());
    }
}

}  // namespace audio
