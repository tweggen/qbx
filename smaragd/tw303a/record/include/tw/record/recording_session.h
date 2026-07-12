#ifndef _RECORDING_SESSION_H_
#define _RECORDING_SESSION_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace audio {

struct RecordingParams {
    std::string inputDeviceId;                    // e.g., "default"
    std::vector<std::string> armedTrackIds;       // track IDs to record on
    std::vector<std::uint32_t> trackChannels;     // per-track channel masks (0 = all)
    std::string projectDirectory;                  // where to save WAV files
    double startTimeSeconds = 0.0;                // locator position (not used yet)
    std::uint32_t sampleRate = 48000;
    std::uint32_t channels = 2;                   // device input channel count
    // Absolute project position (frames) capture starts at; the session
    // advances the playhead from here via onPosition. Supplied by the app
    // (was read from SApplication directly — proposal 14, Phase 0).
    std::uint64_t startLocatorFrames = 0;
};

class RecordingSession {
public:
    RecordingSession();
    ~RecordingSession();

    // Start recording. Returns true on success.
    bool start(const RecordingParams &params);

    // Request stop. Safe to call from any thread.
    void requestStop();

    // Query state (safe from any thread). The UI polls these on its own thread
    // instead of receiving callbacks on the record thread — a worker std::thread
    // must never touch Qt (it crashed in Qt's per-thread teardown when it did).
    bool isRunning() const;
    bool isFinished() const;     // true once the record thread has fully completed
    bool succeeded() const;      // valid once isFinished(): completion vs. error
    double recordedDurationSeconds() const;
    const char *errorMessage() const;  // valid once isFinished()

    // List of created WAV files (returned after recording completes)
    const std::vector<std::string> &createdFiles() const;

    // Get the measured input latency (in frames) from the recording session.
    // Valid after recording completes; 0 if latency could not be determined.
    uint32_t getInputLatencyFrames() const { return inputLatencyFrames_; }

    // Optional callbacks. NOTE: these are invoked ON THE RECORD THREAD, so a
    // handler must be thread-safe and must NOT touch Qt/UI objects. The progress
    // dialog deliberately does not use them — it polls the query methods above
    // from the GUI thread instead.
    std::function<void(double durationSeconds)> onProgress;
    std::function<void(bool success, const char *error)> onComplete;
    // Playhead publication: absolute project position in frames
    // (startLocatorFrames + captured project-rate frames). Called on the
    // RECORD THREAD — handler must be realtime-safe (atomic store, no Qt).
    std::function<void(std::uint64_t absPos)> onPosition;

private:
    void recordThreadMain();

    RecordingParams params_;
    std::atomic<bool> running_{false};
    std::atomic<bool> finished_{false};
    std::atomic<bool> succeeded_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<double> recordedDuration_{0.0};
    std::unique_ptr<std::thread> recordThread_;
    std::string lastError_;
    std::vector<std::string> createdFiles_;
    uint32_t inputLatencyFrames_ = 0;  // Measured input latency during recording
};

}  // namespace audio

#endif
