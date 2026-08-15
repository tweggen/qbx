#ifndef _AUDIO_CAPTURE_BACKEND_H_
#define _AUDIO_CAPTURE_BACKEND_H_

#include "tw/devices/audio_backend.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace audio {

// A device that plays into MEMORY instead of into a speaker.
//
// Why this exists: the qxa suite is structurally render-only, because the only
// backend a headless run could use was NullBackend — and NullBackend::startOutput
// merely sets a flag. It never invokes the render callback, so nothing about the
// PLAYBACK path (AudioEngine::pullBlock, the readahead frontier, the seek on
// start, the stale-page fallback) is observable from a test. Rendering exercises
// RenderSession, which is a different consumer of the same graph; a bug that only
// misplaces audio at playback start is invisible to it.
//
// So: a backend that pumps the callback on a REAL-TIME paced clock and keeps
// every frame the callback produced. The recording can then be written out and
// decoded (assert-source-position) exactly like a render.
//
// THE PACING IS DELIBERATE AND IT IS REAL TIME. A virtual clock — "call the
// callback as fast as the readahead can serve it" — would make every run
// deterministic by removing precisely the pressure we are hunting: the races
// between the readahead, the revalidation workers and an edit are all about the
// callback arriving BEFORE the page is ready. SMARAGD_CAPTURE_SPEED multiplies
// the rate for a smoke run (4.0 == four times faster than real time) but the
// deadline discipline stays: the loop never waits for audio to be ready.
//
// Threading: the pump thread owns the callback invocation and holds NO lock
// across it (same discipline as the platform backends, whose callbacks run on a
// driver thread that owns nothing of ours). The append into the recording takes
// a short mutex AFTER the callback has returned.
class CaptureBackend : public AudioBackend {
public:
    // One recording: interleaved float frames plus the format they were
    // delivered in. `droppedFrames` counts frames the callback produced that
    // the cap below refused to keep, so a truncated recording can never be
    // mistaken for a short one.
    struct CaptureBuffer {
        std::uint32_t      sampleRate = 0;
        std::uint32_t      channels   = 0;
        std::vector<float> samples;              // interleaved, channels-major
        std::uint64_t      droppedFrames = 0;

        std::size_t frames() const
        {
            return channels ? samples.size() / channels : 0;
        }
    };

    // When one block of frames was handed to the "device". `firstFrame` is the
    // recording-relative index of the block's first frame, so the pair is one
    // sample of the map host time -> project frame.
    //
    // Why it exists (proposal 37 D6, review #12): the MIDI capture backend
    // records the host time a message reached the wire and NOTHING else, so a
    // MIDI-out assertion needs an INDEPENDENT way to say where the playhead was
    // at that instant. This log is it — produced by the audio pump, from the
    // same steady clock, with no knowledge of the pump under test. Asking the
    // MIDI pump's own model where it thought it was would prove nothing.
    struct BlockStamp {
        std::int64_t hostTimeNs = 0;
        std::int64_t firstFrame = 0;
    };

    // Recording cap. Five minutes of stereo float at 48 kHz is ~275 MB, which is
    // far more than any case needs and still bounded — a runaway case truncates
    // and logs instead of eating the machine.
    static constexpr std::uint64_t kMaxCaptureSeconds = 300;

    CaptureBackend();
    ~CaptureBackend() override;

    int  openDevice(const std::string &deviceName = "default",
                    std::uint32_t preferredRate = 0) override;
    int  closeDevice() override;
    int  startOutput() override;
    int  stopOutput() override;
    bool isRunning() const override { return running_.load(std::memory_order_relaxed); }

    void setRenderCallback(RenderCallback cb) override;
    AudioConfig getConfig() const override { return config_; }

    // Nothing constrains this device, so it opens at whatever was asked for and
    // the speaker's resampler stays a passthrough (see openDevice).
    std::vector<std::uint32_t> supportedRates() const override { return {}; }
    std::vector<AudioDeviceInfo> enumerateDevices() const override;

    const char *name() const override { return "capture"; }

    // A COPY of the recording so far, taken under the pump's lock. A copy and
    // not a reference: the pump appends while a caller reads, and a vector that
    // reallocates under a reader is a dangling span. The recordings a test
    // writes out are seconds long, so the copy is cheap next to the WAV encode
    // that follows it.
    CaptureBuffer capturedAudio() const;

    // Frames recorded so far — a cheap poll that does not copy the samples.
    std::size_t capturedFrames() const;

    // Drop the recording. Called by startOutput() so that captured frame 0 is
    // always the first frame of the CURRENT playback session: a case that plays,
    // edits, and plays again wants the second pass, and would otherwise have to
    // do arithmetic over the first one.
    void clearCapture();

    // A COPY of the block log, under the same lock and for the same reason as
    // capturedAudio(). Cleared together with the recording.
    std::vector<BlockStamp> capturedBlockLog() const;

    // Which project frame was being delivered at `hostTimeNs`, by PIECEWISE
    // LINEAR interpolation over the block log. Outside the log it extrapolates
    // along the nearest segment (or, with a single stamp, along the nominal
    // rate), so an event just before the first block or just after the last one
    // still maps — it is a measurement, and clamping would silently turn a real
    // offset into zero. Returns -1 only when there is no log at all.
    std::int64_t frameAtHostTime(std::int64_t hostTimeNs) const;

    // The same arithmetic over a caller-supplied log, so it can be tested
    // against a synthetic one without a running pump. `nominalFramesPerSecond`
    // is used only when a single stamp leaves no slope to read.
    static std::int64_t frameAtHostTime(const std::vector<BlockStamp> &log,
                                        std::int64_t hostTimeNs,
                                        double nominalFramesPerSecond);

private:
    void pumpLoop();

    AudioConfig       config_;
    RenderCallback    callback_;
    mutable std::mutex callbackMutex_;   // guards callback_ (set/read across threads)

    std::thread       pump_;
    std::atomic<bool> running_{ false };
    std::atomic<bool> pumpRunning_{ false };

    mutable std::mutex captureMutex_;    // guards capture_ AND blockLog_
    CaptureBuffer     capture_;
    std::vector<BlockStamp> blockLog_;

    // Real-time multiplier from SMARAGD_CAPTURE_SPEED, read once per instance.
    double            speed_ = 1.0;
};

}  // namespace audio

#endif
