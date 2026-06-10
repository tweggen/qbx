#ifndef _AUDIO_INPUT_H_
#define _AUDIO_INPUT_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "twformat.h"

namespace audio {

struct AudioInputConfig {
    std::uint32_t sampleRate = 48000;
    std::uint32_t channels = 2;
    std::uint32_t bufferFrames = 1024;
    twSampleType sampleType = twSampleType::Float32;
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

    // List available input devices
    virtual std::vector<AudioInputDeviceInfo> listDevices() const = 0;

    // Get error message from last failed operation
    virtual const char *errorMessage() const = 0;
};

std::unique_ptr<AudioInput> createAudioInput();

}  // namespace audio

#endif
