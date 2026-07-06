#ifndef _TWSPEAKER_H_
#define _TWSPEAKER_H_

#include "twcomponent.h"
#include "twresampler.h"
#include "audio/audio_backend.h"
#include "audio/audio_engine.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Playback output state machine (Phase 6b: deferred backend startup until buffer ready)
enum class OutputState {
    STOPPED = 0,    // No output, no engine
    OPENING = 1,    // Device opening, engine creating
    BUFFERING = 2,  // Readahead buffering, callback not started
    PLAYING = 3,    // Audio flowing
    STOPPING = 4    // Shutting down
};

class twSpeaker
    : public twComponent
{
    Q_OBJECT
    virtual void reset() override;
private:
    std::unique_ptr<audio::AudioBackend> backend_;
    // CRITICAL: Changed from unique_ptr to shared_ptr to prevent use-after-free.
    // The render callback captures 'this' and must access audioEngine_ safely even after
    // stopOutput() is called. With shared_ptr, stopping the backend stops the callback
    // thread, but local copies of the shared_ptr in flight keep the engine alive.
    // This prevents crashes when stopOutput() is called concurrently with setCycle().
    std::shared_ptr<audio::AudioEngine> audioEngine_;
    // Atomic so isPlaying() is a lock-free read. The check-and-act transitions
    // (start/stop) are serialised by outputMutex_ below — atomicity of the flag
    // alone doesn't make "if (!isPlaying_) return; …flip + drive the backend"
    // safe against a concurrent/re-entrant caller.
    std::atomic<bool> isPlaying_;
    // Serialises startOutput()/stopOutput() so a play can't interleave with a
    // stop (and a double stop is idempotent). The backend render thread never
    // takes this lock, so holding it across backend_->stopOutput()'s join is
    // deadlock-free.
    std::mutex outputMutex_;
    std::string outputDeviceId_ = "default";

    // Cycle (loop) playback. When enabled with a valid range, the render
    // callback seamlessly wraps the play cursor from loopEnd_ back to loopStart_
    // (the end frame itself is not played, so the loop is [start, end)). The
    // atomics are written from the UI thread (setCycle) and read on the audio
    // thread, so playback never tears on a mid-buffer toggle.
    std::atomic<bool>     cycleEnabled_{ false };
    std::atomic<offset_t> loopStart_{ 0 };
    std::atomic<offset_t> loopEnd_{ 0 };
    // Output frames produced per input (synth-time) frame consumed; 1.0 when the
    // device opened at the graph rate (resampler is a passthrough). Used to bound
    // a pull so it doesn't overshoot the loop end. Set in startOutput().
    double                rateRatio_ = 1.0;

    // Phase 6b: Output state machine (deferred backend startup until buffer ready)
    std::atomic<OutputState> outputState_{OutputState::STOPPED};
    std::thread bufferingTask_;          // Background thread monitoring readahead progress
    std::atomic<bool> bufferingTaskRunning_{false};  // Signal to stop buffering task

    // Helper: Background task that waits for readahead buffer, then starts backend output
    void monitorReadaheadBuffer();

protected:
    // Phase 3: IOVector-based interface (type-safe, page-backed)
    virtual length_t calcOutputTo(IOVector& dest, idx_t idx) override;

public:
    ~twSpeaker();
    twSpeaker(tw303aEnvironment &);

    virtual void createOutputLatches(void);

    virtual const char *getInputName(idx_t)  const { return nullptr; }
    virtual const char *getOutputName(idx_t) const { return nullptr; }
    virtual idx_t getNInputs()  const { return 2; }
    virtual idx_t getNOutputs() const { return 0; }

    void setBufferSize(length_t) {}

    bool isPlaying();

    // Enable/disable cycle (loop) playback and set its bounds (sample frames in
    // synth time). An empty/inverted range (end <= start) disables cycling. Safe
    // to call at any time, including during playback.
    void setCycle( bool enabled, offset_t startFrame, offset_t endFrame );

    // Output device selection (for a device-picker UI). The id is a backend
    // device id from outputDevices(); "default" / empty means the system
    // default endpoint. Takes effect on the next startOutput().
    void setOutputDevice( const std::string &id );
    const std::string &outputDevice() const { return outputDeviceId_; }
    std::vector<audio::AudioDeviceInfo> outputDevices() const;

    // Get the audio backend for querying configuration, latency, and buffer sizes.
    audio::AudioBackend *getBackend() const { return backend_.get(); }

    // Get the audio engine for readahead control and playback state management
    audio::AudioEngine *getAudioEngine() const { return audioEngine_.get(); }

    // Get current output state (for UI status line display)
    OutputState getOutputState() const { return outputState_.load(std::memory_order_relaxed); }

public slots:
    void startOutput();
    void stopOutput();
};

#endif
