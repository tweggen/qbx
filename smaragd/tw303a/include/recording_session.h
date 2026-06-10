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
    std::string projectDirectory;                  // where to save WAV files
    double startTimeSeconds = 0.0;                // locator position (not used yet)
    std::uint32_t sampleRate = 48000;
    std::uint32_t channels = 2;
};

class RecordingSession {
public:
    RecordingSession();
    ~RecordingSession();

    // Start recording. Returns true on success.
    bool start(const RecordingParams &params);

    // Request stop. Safe to call from any thread.
    void requestStop();

    // Query state (safe from any thread)
    bool isRunning() const;
    double recordedDurationSeconds() const;
    const char *errorMessage() const;

    // List of created WAV files (returned after recording completes)
    const std::vector<std::string> &createdFiles() const;

    // Callbacks (called from record thread, must be thread-safe)
    // Called every ~0.1s with current duration
    std::function<void(double durationSeconds)> onProgress;

    // Called when recording completes
    // success=true: normal completion
    // success=false: error occurred
    std::function<void(bool success, const char *error)> onComplete;

private:
    void recordThreadMain();

    RecordingParams params_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<double> recordedDuration_{0.0};
    std::unique_ptr<std::thread> recordThread_;
    std::string lastError_;
    std::vector<std::string> createdFiles_;
};

}  // namespace audio

#endif
