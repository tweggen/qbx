#include "tw/record/recording_session.h"

#include "tw/devices/audio_input.h"
#include "tw/sinks/audio_file_writer.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <vector>

// Diagnostic output to stderr, tagged with file + function, flushed immediately so
// the last line before a crash/hang is never lost in a buffer. Mirrors the
// rate-diagnostic style in twspeaker.cc startOutput() — used here to pin down the
// "recorded take plays back too fast" bug (capture rate vs project rate).
#define RECSESS_LOG( fmt, ... )                                                  \
    do {                                                                         \
        fprintf( stderr, "recording_session.cc:%s: " fmt "\n", __func__,         \
                 ##__VA_ARGS__ );                                                \
        fflush( stderr );                                                        \
    } while( 0 )

namespace audio {

namespace {

// Filter selected channels from an interleaved float buffer.
// If channelMask is 0, all channels are passed through.
// Otherwise, channelMask is a bitmask where bit n enables channel n.
std::vector<float> filterChannels(const float *data, std::size_t frameCount,
                                   std::uint32_t sourceChannels, std::uint32_t channelMask) {
    std::vector<float> result;

    // If no mask (0), pass through all channels
    if (channelMask == 0) {
        result.assign(data, data + frameCount * sourceChannels);
        return result;
    }

    // Count how many channels are selected
    std::uint32_t selectedChannels = 0;
    std::vector<std::uint32_t> selectedIndices;
    for (std::uint32_t ch = 0; ch < sourceChannels; ++ch) {
        if (channelMask & (1U << ch)) {
            selectedChannels++;
            selectedIndices.push_back(ch);
        }
    }

    if (selectedChannels == 0) {
        // No channels selected - return silence
        result.resize(frameCount * 1, 0.0f);  // At least mono
        return result;
    }

    // Extract selected channels
    result.reserve(frameCount * selectedChannels);
    for (std::size_t f = 0; f < frameCount; ++f) {
        for (std::uint32_t ch : selectedIndices) {
            result.push_back(data[f * sourceChannels + ch]);
        }
    }

    return result;
}

// Stateful streaming linear resampler for interleaved float frames. The input
// backend normally delivers frames already at the project rate (WASAPI shared-mode
// AUTOCONVERTPCM — see WASAPIInput::openDevice), so this is a passthrough in the
// common case. It only does real work on the fallback path, where a driver refused
// auto-conversion and we capture at the device's native mix rate and convert here
// before writing the WAV (so recorded files always match the project rate). Linear
// quality matches the engine's twResampler bar; that converter pulls from a
// streaming-latch component and doesn't fit this push-buffer path, so this is a
// small standalone version. Passthrough when the rates already match.
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

    // Capture the input latency for later sync compensation (when playback and
    // recording are simultaneous).
    inputLatencyFrames_ = input->getLatencyFrames();

    // Rate diagnostic for the "take plays back too fast" bug. If device != target
    // but the resampler reports passthrough below, the device->project conversion
    // failed to engage and the WAV will carry device-rate content under a
    // project-rate header — exactly the 44.1k-played-as-48k symptom.
    RECSESS_LOG( "rate diag — capture device=%u Hz, project(target)=%u Hz, "
                 "channels=%u, requested=%u Hz",
                 (unsigned) deviceRate, (unsigned) targetRate,
                 (unsigned) channels, (unsigned) params_.sampleRate );

    // Create per-track WAV writers and filenames
    std::vector<std::unique_ptr<AudioFileWriter>> writers;
    std::vector<std::string> filenames;
    std::vector<std::uint32_t> outputChannelCounts;  // channels per track after filtering

    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) %
              1000;

    // Create a writer for each armed track
    for (std::size_t trackIdx = 0; trackIdx < params_.armedTrackIds.size(); ++trackIdx) {
        std::stringstream filenameSS;
        filenameSS << params_.projectDirectory << "/";
        filenameSS << std::put_time(std::localtime(&time), "%Y%m%d_%H%M%S");
        filenameSS << "_" << std::setfill('0') << std::setw(3) << ms.count();
        filenameSS << "_" << params_.armedTrackIds[trackIdx] << ".wav";

        std::string filename = filenameSS.str();
        filenames.push_back(filename);
        createdFiles_.push_back(filename);

        // Determine output channel count for this track
        std::uint32_t trackChannelMask = (trackIdx < params_.trackChannels.size()) ?
                                         params_.trackChannels[trackIdx] : 0;
        std::uint32_t outChannels = channels;  // default: all channels

        if (trackChannelMask != 0) {
            // Count selected channels
            outChannels = 0;
            for (std::uint32_t ch = 0; ch < channels; ++ch) {
                if (trackChannelMask & (1U << ch)) {
                    outChannels++;
                }
            }
            if (outChannels == 0) outChannels = 1;  // At least mono
        }
        outputChannelCounts.push_back(outChannels);

        // Create WAV writer
        auto writer = createAudioFileWriter(AudioFormat::WAV);
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
        fileConfig.channels = outChannels;
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

        writers.push_back(std::move(writer));
    }

    // Start input capture
    if (input->startCapture() < 0) {
        lastError_ = std::string("Failed to start capture: ") + input->errorMessage();
        // Close all writers before returning on error
        for (auto &writer : writers) {
            writer->close();
        }
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
    RECSESS_LOG( "resampler %u Hz -> %u Hz: %s",
                 (unsigned) deviceRate, (unsigned) targetRate,
                 resampler.passthrough() ? "passthrough (no conversion)" : "active" );
    auto lastProgressTime = std::chrono::steady_clock::now();

    bool success = true;
    std::string errorMsg;

    // Advance the playback locator in lock-step with what we capture, so the
    // playhead tracks the recording. Start from the locator position the user
    // armed at (supplied via params — the engine never asks the app); advance
    // by output (project-rate) frames written. onPosition must be a realtime
    // store only — never emit Qt signals from this worker thread (a main-thread
    // QTimer turns these stores into playhead repaints; see SApplication::pumpLocator).
    const std::uint64_t startLocator = params_.startLocatorFrames;
    std::uint64_t recordedProjectFrames = 0;

    // Empirical capture-rate check (for the "take plays back too low" bug). We
    // count the input frames the device actually hands us and divide by real
    // wall-clock elapsed. If that effective rate differs from the deviceRate we
    // queried (e.g. we asked GetMixFormat which said 44100, but the shared engine
    // actually streams 48000), our resampler is converting from the wrong source
    // rate — the file then carries the wrong number of frames under a 48000 header.
    const auto captureStart = std::chrono::steady_clock::now();
    long long totalInputFrames = 0;

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
                totalInputFrames += framesRead;
                // Resample device-rate capture to the project rate, then write to per-track files.
                const float *outData = buffer.data();
                std::int32_t outFrames = framesRead;
                if (!resampler.passthrough()) {
                    std::size_t n = resampler.process(buffer.data(),
                                                      static_cast<std::size_t>(framesRead),
                                                      resampled);
                    outData = resampled.data();
                    outFrames = static_cast<std::int32_t>(n);
                }

                // Write to per-track WAV files with channel filtering (a chunk can resample to 0 output frames — skip it)
                if (outFrames > 0) {
                    for (std::size_t trackIdx = 0; trackIdx < writers.size(); ++trackIdx) {
                        std::uint32_t trackChannelMask = (trackIdx < params_.trackChannels.size()) ?
                                                         params_.trackChannels[trackIdx] : 0;

                        // Filter channels if needed
                        std::vector<float> filtered = filterChannels(outData, outFrames, channels, trackChannelMask);

                        if (!writers[trackIdx]->write(filtered.data(), outFrames)) {
                            success = false;
                            errorMsg = std::string("Write error on track ") + params_.armedTrackIds[trackIdx] +
                                     std::string(": ") + writers[trackIdx]->errorMessage();
                            break;
                        }
                    }
                    if (!success) break;
                }

                // Update duration from the input frames at the device rate (real
                // elapsed time, independent of the resampling ratio).
                recordedDuration_.store(
                    recordedDuration_.load() + static_cast<double>(framesRead) / deviceRate
                );

                // Advance the playhead by the project-rate frames just captured.
                if (outFrames > 0) {
                    recordedProjectFrames += static_cast<std::uint64_t>(outFrames);
                    if (onPosition) onPosition(startLocator + recordedProjectFrames);
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

    // Effective capture rate: frames the device actually delivered per real second.
    // If this is ~48000 while we assumed (and resampled from) deviceRate=44100, the
    // shared engine was streaming 48k and we double-converted — the smoking gun for
    // the "take plays back ~8.8% too low" bug.
    {
        double wall = std::chrono::duration_cast<std::chrono::duration<double>>(
                          std::chrono::steady_clock::now() - captureStart ).count();
        double effRate = wall > 0.0 ? (double) totalInputFrames / wall : 0.0;
        RECSESS_LOG( "capture-rate check — assumed deviceRate=%u Hz, "
                     "MEASURED effective=%.1f Hz over %.3fs (%lld input frames). "
                     "ratio meas/assumed=%.4f",
                     (unsigned) deviceRate, effRate, wall, totalInputFrames,
                     deviceRate > 0 ? effRate / deviceRate : 0.0 );
    }

    // Stop and cleanup
    input->stopCapture();
    input->closeDevice();

    // Close all per-track writers
    for (std::size_t trackIdx = 0; trackIdx < writers.size(); ++trackIdx) {
        if (!writers[trackIdx]->close()) {
            if (success) {  // Only override error if we didn't already have one
                success = false;
                errorMsg = std::string("Failed to close WAV file for track ") +
                          params_.armedTrackIds[trackIdx];
            }
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
