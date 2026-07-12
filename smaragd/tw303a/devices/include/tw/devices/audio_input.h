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
    std::uint32_t channels = 2;
    std::uint32_t bufferFrames = 1024;
    twSampleType sampleType = twSampleType::Float32;
    // Total input latency (device + driver + resampler) in frames.
    // Measured/calculated after device is opened; 0 if not yet determined.
    std::uint32_t inputLatencyFrames = 0;
};

struct AudioInputDeviceInfo {
    std::string id;           // backend-specific device handle
    std::string name;         // human-readable label
    std::uint32_t channels;   // number of input channels
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
    virtual std::int32_t read(float *interleaved, std::size_t frameCount) = 0;

    // Query current configuration
    virtual const AudioInputConfig &getConfig() const = 0;

    // Get the total input latency (device + driver + resampler) in frames.
    // Returns 0 if not yet determined or if getConfig().inputLatencyFrames is not set.
    virtual uint32_t getLatencyFrames() const { return getConfig().inputLatencyFrames; }

    // Get the list of selectable buffer sizes (in frames) for this backend.
    // Empty list means buffer size is not user-selectable (fixed by device/OS).
    // Called after openDevice(); may probe the device but must not disturb capture.
    virtual std::vector<uint32_t> getAvailableBufferSizes() const { return {}; }

    // Request a specific buffer size (in frames). Returns 0 on success, -1 on error.
    // Only valid after openDevice(). Requires capture to be stopped.
    // The backend may return a different size than requested (if the exact size is
    // not supported); call getConfig().bufferFrames to get the actual size.
    virtual int setBufferSize(uint32_t frameCount) { return -1; }

    // List available input devices
    virtual std::vector<AudioInputDeviceInfo> listDevices() const = 0;

    // Get error message from last failed operation
    virtual const char *errorMessage() const = 0;
};

std::unique_ptr<AudioInput> createAudioInput();

}  // namespace audio

#endif
