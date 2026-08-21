#ifndef _AUDIO_INPUT_H_
#define _AUDIO_INPUT_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "tw/core/twformat.h"

namespace audio {

struct AudioInputConfig {
    std::uint32_t sampleRate = 48000;
    // The width of the stream AS OPENED — what read() interleaves. On ASIO
    // that is the DEMANDED channel set (grow-only, one channel until somebody
    // asks for more), NOT what the interface has: see `deviceChannels`.
    std::uint32_t channels = 2;
    std::uint32_t bufferFrames = 1024;
    twSampleType sampleType = twSampleType::Float32;
    // Total input latency (device + driver + resampler) in frames.
    // Measured/calculated after device is opened; 0 if not yet determined.
    std::uint32_t inputLatencyFrames = 0;
    // How many input channels the DEVICE HAS, which on ASIO is a different
    // number from `channels` above and is the one a channel PICKER wants: a
    // 16-input interface whose stream is open on input 1 alone reports
    // `channels == 1` and `deviceChannels == 16`. 0 means the backend does not
    // report it separately — read it through AudioInput::deviceInputChannels(),
    // which falls back to `channels` (correct for every backend that opens the
    // endpoint's full width whether anyone asked or not).
    std::uint32_t deviceChannels = 0;
};

struct AudioInputDeviceInfo {
    std::string id;           // backend-specific device handle
    std::string name;         // human-readable label
    std::uint32_t channels;   // number of input channels
    // The endpoint's SHARED-MODE MIX RATE as Windows/the OS reports it, 0 when
    // unknown. Surfaced because an input endpoint declared at one rate while
    // the output endpoint is declared at another shares ONE hardware clock:
    // the OS then resamples one side and MISREPORTS its clock, and no amount
    // of arithmetic downstream can recover from being lied to about the rate.
    // Showing it at the point of choice is what makes that visible.
    std::uint32_t sampleRate = 0;
};

// What the capture thread and its ring have done since the device was opened
// (proposal 21 L0). Every counter is a plain relaxed atomic read: a meter, a
// log line and a test all read them, none of them synchronise on them.
//
//   framesPushed   frames the capture thread put into the ring
//   framesPopped   frames read() handed out
//   overrunFrames  frames the DEVICE produced that the ring could not hold
//                  (the consumer is not keeping up) — audio that was lost
//   underrunFrames frames a NON-EMPTY ring could not satisfy in one read()
//                  (a hole in the consumer's stream); an idle device with
//                  nothing at all to give is not counted here
//   captureWakeups times the capture thread woke with frames to move
struct AudioInputStats {
    std::uint64_t framesPushed   = 0;
    std::uint64_t framesPopped   = 0;
    std::uint64_t overrunFrames  = 0;
    std::uint64_t underrunFrames = 0;
    std::uint64_t captureWakeups = 0;
};

class AudioInput {
public:
    virtual ~AudioInput() = default;

    // Open input device. Returns 0 on success, -1 on error.
    virtual int openDevice(const std::string &deviceId = "default",
                           std::uint32_t preferredRate = 0) = 0;

    // Close input device. Returns 0 on success.
    virtual int closeDevice() = 0;

    // Start capturing audio. Returns 0 on success.
    virtual int startCapture() = 0;

    // Stop capturing audio. Returns 0 on success.
    virtual int stopCapture() = 0;

    // Read frames from input buffer. Returns number of frames actually read
    // (may be less if buffer underflow). Returns -1 on error.
    //
    // Proposal 21 L0: this is a RING POP, not a device poll. The device is
    // drained by a capture thread that pushes WHOLE packets into an SPSC ring
    // (see audio_ring.h), so a caller asking for fewer frames than the device
    // produced no longer loses the remainder — it stays in the ring for the
    // next call. Non-blocking as before: 0 means "nothing available yet".
    // ONE consumer thread only.
    virtual std::int32_t read(float *interleaved, std::size_t frameCount) = 0;

    // Capture-thread / ring diagnostics. Zero for a backend that has neither.
    virtual AudioInputStats stats() const { return {}; }

    // Query current configuration
    virtual const AudioInputConfig &getConfig() const = 0;

    // Get the total input latency (device + driver + resampler) in frames.
    // Returns 0 if not yet determined or if getConfig().inputLatencyFrames is not set.
    virtual uint32_t getLatencyFrames() const { return getConfig().inputLatencyFrames; }

    // How many input channels the DEVICE HAS — what a channel PICKER must be
    // sized from. NOT getConfig().channels, which is the width of the stream
    // as opened: on ASIO the channel set is demand-driven and grow-only, so a
    // 16-input interface reports channels == 1 until something asks for more,
    // and a picker built from that offers exactly one channel forever.
    virtual std::uint32_t deviceInputChannels() const
    {
        const AudioInputConfig &c = getConfig();
        return c.deviceChannels ? c.deviceChannels : c.channels;
    }

    // Get the list of selectable buffer sizes (in frames) for this backend.
    // Empty list means buffer size is not user-selectable (fixed by device/OS).
    // Called after openDevice(); may probe the device but must not disturb capture.
    virtual std::vector<uint32_t> getAvailableBufferSizes() const { return {}; }

    // Ask the device to make these INPUT CHANNELS available, as a bitmask
    // where bit n means input n — the same convention as
    // SObject::recordingChannels_, which the capture bridge applies per sink.
    //
    // Default: a NO-OP returning 0, because every backend but ASIO opens the
    // endpoint's channels whether anyone asked or not and has nothing to do
    // here. ASIO is different: the channel set is fixed at createBuffers time
    // and changing it needs the driver STOPPED, so it has to be told what to
    // open before it opens (proposal 35 Phase 3).
    //
    // Contract for a backend that implements it:
    //   - GROW-ONLY. The set never shrinks, so a channel armed once stays
    //     cheap to arm again and disarming never disturbs a running stream.
    //   - It NEVER restarts a running stream. If capture is running and the
    //     mask asks for something not yet open, the request is recorded and
    //     applied at the next start — the same "takes effect on next Play"
    //     model a device change already uses. Returns 1 to say so.
    //   - Returns 0 when the request is already satisfied or was applied.
    virtual int requestChannels(std::uint64_t /*mask*/) { return 0; }

    // Request a specific buffer size (in frames). Returns 0 on success, -1 on error.
    // Only valid after openDevice(). Requires capture to be stopped.
    // The backend may return a different size than requested (if the exact size is
    // not supported); call getConfig().bufferFrames to get the actual size.
    virtual int setBufferSize(uint32_t frameCount) { return -1; }

    // List available input devices
    virtual std::vector<AudioInputDeviceInfo> listDevices() const = 0;

    // Get error message from last failed operation
    virtual const char *errorMessage() const = 0;

    // Which implementation this is: "wasapi" | "alsa" | "coreaudio" | "null" |
    // "file". Mirrors MidiOutput::backendName(); it is what makes the env
    // selection assertable without opening anything.
    virtual const char *backendName() const = 0;
};

// Selected by SMARAGD_AUDIO_INPUT_BACKEND ahead of the platform, exactly like
// SMARAGD_AUDIO_BACKEND does for the output side (proposal 21 L0, design D9):
//
//   file:<path>   FileAudioInput — replay a WAV in 1024-frame blocks, paced on
//                 MidiOutScheduler::hostNowNs(), through a capture thread and a
//                 ring like any device. This is what makes a monitoring or
//                 recording case assertable at all.
//   null          NullInput — silence, no device, no thread. The --test-case
//                 DEFAULT (set in main.cpp unless already set): a headless
//                 suite must not open the developer's microphone.
//   default / ""  the compile-time platform pick (WASAPI / ALSA / CoreAudio).
//
// Unknown values warn and fall back to the platform, never to a null pointer.
// Read per call — unlike the output backend there is no single mint point.
//
// Two companion variables shape the file backend, so a case can express them
// without a second env grammar inside the path:
//   SMARAGD_AUDIO_INPUT_LOOP=0|1              (default 1: loop at end)
//   SMARAGD_AUDIO_INPUT_LATENCY_FRAMES=<n>    reported inputLatencyFrames
std::unique_ptr<AudioInput> createAudioInput();

// The same selection, explicitly. `backend` empty == read the environment.
std::unique_ptr<AudioInput> createAudioInput(const std::string &backend);

}  // namespace audio

#endif
