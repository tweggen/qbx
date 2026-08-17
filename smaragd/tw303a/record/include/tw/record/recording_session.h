#ifndef _RECORDING_SESSION_H_
#define _RECORDING_SESSION_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace audio {

class CaptureBridge;

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

/**
 * A CONSUMER OF THE CAPTURE BRIDGE (proposal 21 L3a, design D7).
 *
 * The public shape below is unchanged — the app starts a session, polls it and
 * reads createdFiles() exactly as before — but there is no capture loop here
 * any more. CaptureBridge owns the device, the ring drain, the growing capture
 * pages and the WAV writers; this class is the transport-scoped wrapper that
 * names the files, publishes the playhead and the progress, and finalises.
 *
 * What went away: the 1 ms poll. The session's own thread now WAITS on a
 * condition variable until stop is requested (waking every 100 ms only to emit
 * progress); the playhead comes from the bridge's per-batch callback, on the
 * bridge thread, as an atomic store. Nothing here touches Qt.
 */
class RecordingSession {
public:
    RecordingSession();
    ~RecordingSession();

    // Start recording. Returns true on success.
    bool start(const RecordingParams &params);

    // Request stop. Safe to call from any thread; never blocks. The session
    // thread does the finalising (which does block: the files are completed
    // out of the capture pages).
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

    // The bridge behind this session, or null before start(). Its growing
    // capture source is what L3b draws and places while the capture is still
    // running; it stays readable after stop().
    CaptureBridge *bridge() const { return bridge_.get(); }

    // Optional callbacks. NOTE: these are invoked ON A WORKER THREAD, so a
    // handler must be thread-safe and must NOT touch Qt/UI objects. The progress
    // dialog deliberately does not use them — it polls the query methods above
    // from the GUI thread instead.
    std::function<void(double durationSeconds)> onProgress;
    std::function<void(bool success, const char *error)> onComplete;
    // Playhead publication: absolute project position in frames
    // (startLocatorFrames + captured project-rate frames). Called on the
    // BRIDGE THREAD — handler must be realtime-safe (atomic store, no Qt).
    std::function<void(std::uint64_t absPos)> onPosition;

private:
    void sessionThreadMain();
    void joinSessionThread();
    void markFinished(bool ok);

    RecordingParams params_;
    std::atomic<bool> running_{false};
    std::atomic<bool> finished_{false};
    std::atomic<bool> succeeded_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<double> recordedDuration_{0.0};

    std::unique_ptr<CaptureBridge> bridge_;
    std::unique_ptr<std::thread> sessionThread_;
    std::mutex mutex_;
    std::condition_variable cv_;

    std::string lastError_;
    std::vector<std::string> createdFiles_;
    uint32_t inputLatencyFrames_ = 0;  // Measured input latency during recording
};

}  // namespace audio

#endif
