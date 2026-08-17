#ifndef _TW_FILE_INPUT_H_
#define _TW_FILE_INPUT_H_

#include "tw/devices/audio_input.h"
#include "tw/devices/audio_ring.h"

#include "precise_waiter.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace audio {

// A WAV file pretending to be a capture device (proposal 21 L0, design D9).
//
// Selected with SMARAGD_AUDIO_INPUT_BACKEND=file:<path>. It is the input half
// of what CaptureBackend is for the output: the thing that makes a live /
// monitoring / recording path assertable headlessly, with content the case
// wrote and therefore knows.
//
// It is a REAL device, not a shortcut:
//   - its own capture thread, its own SPSC ring, the same read() ring pop;
//   - blocks of kBlockFrames (1024) frames, PACED on the shared steady clock
//     MidiOutScheduler::hostNowNs() through the same high-resolution waitable
//     timer the MIDI sender uses (PreciseWaiter). The pacing is real time on
//     purpose, exactly as CaptureBackend's is (devices inv. 7): a clock that
//     handed the whole file over at once would remove the very pressure — the
//     consumer arriving before the frames do — that a live-input case exists
//     to find;
//   - block i is delivered at t0 + (i+1)*period, because a device cannot hand
//     over audio it has not recorded yet;
//   - a configurable reported inputLatencyFrames, so a case can assert that a
//     latency number travels correctly without owning any hardware.
//
// Threading: the capture thread is the ring's ONLY producer and touches
// nothing else; read() (any single consumer) is the only popper. openDevice /
// start / stop / close are control-plane calls from one thread, as for every
// AudioInput.
class FileAudioInput : public AudioInput {
public:
    // Frames per delivered block. 1024 is the capture backend's block on the
    // output side; keeping the two equal means a round-trip case's two clocks
    // tick at the same rate.
    static constexpr std::size_t kBlockFrames = 1024;

    explicit FileAudioInput(std::string path);
    ~FileAudioInput() override;

    // Loop at end (default) or stop and deliver nothing further.
    void setLoop(bool loop) { loop_ = loop; }
    bool loop() const { return loop_; }

    // What getLatencyFrames() will report. Purely declarative — the file
    // backend has no latency of its own; this is how a case supplies one.
    void setReportedLatencyFrames(std::uint32_t frames);

    // The host time the capture thread's schedule is anchored on (0 before
    // startCapture). Block i was due at captureStartHostNs() + (i+1)*period.
    std::int64_t captureStartHostNs() const
    { return startHostNs_.load(std::memory_order_acquire); }

    // Blocks pushed so far, and whether the file has been played out (only
    // meaningful with looping off).
    std::uint64_t blocksDelivered() const
    { return blocksDelivered_.load(std::memory_order_relaxed); }
    bool atEnd() const { return atEnd_.load(std::memory_order_relaxed); }

    // The host time each block was PUSHED at, in block order — the file
    // backend's version of CaptureBackend's {hostTimeNs, firstFrame} log
    // (devices inv. 9), and for the same reason: the pacing of a device is a
    // wall-clock property, so the only way to hold it to a number is to record
    // when delivery actually happened. Stamped by the capture thread the
    // instant it wakes, before the push; capped, so a long run cannot grow it
    // without bound. `count` is how many entries are valid.
    static constexpr std::size_t kMaxBlockLog = 8192;
    std::size_t blockLog(std::vector<std::int64_t> &out) const;

    // The decoded file, interleaved float, for a test that wants to compare
    // what came out of the ring against what went in. Valid after openDevice().
    const std::vector<float> &fileSamples() const { return samples_; }
    std::size_t fileFrames() const { return fileFrames_; }

    int openDevice(const std::string &deviceId = "default",
                   std::uint32_t preferredRate = 0) override;
    int closeDevice() override;
    int startCapture() override;
    int stopCapture() override;
    std::int32_t read(float *interleaved, std::size_t frameCount) override;

    const AudioInputConfig &getConfig() const override { return config_; }
    AudioInputStats stats() const override;
    std::vector<AudioInputDeviceInfo> listDevices() const override;
    const char *errorMessage() const override { return lastError_.c_str(); }
    const char *backendName() const override { return "file"; }

private:
    void captureThreadMain();

    std::string        path_;
    std::string        lastError_;
    AudioInputConfig   config_;

    std::vector<float> samples_;       // interleaved, whole file
    std::size_t        fileFrames_ = 0;

    AudioRing          ring_;
    PreciseWaiter      waiter_;

    std::thread        thread_;
    std::atomic<bool>  running_{ false };
    std::atomic<bool>  atEnd_{ false };
    std::atomic<std::int64_t>  startHostNs_{ 0 };
    std::atomic<std::uint64_t> blocksDelivered_{ 0 };
    std::atomic<std::uint64_t> wakeups_{ 0 };

    // Written ONLY by the capture thread (index < the count it publishes), read
    // by anyone after the fact; blockLogCount_ is the release that makes an
    // entry visible. Sized once in startCapture, never resized.
    std::vector<std::int64_t>  blockLog_;
    std::atomic<std::size_t>   blockLogCount_{ 0 };

    bool               loop_ = true;
    bool               open_ = false;
};

}  // namespace audio

#endif
