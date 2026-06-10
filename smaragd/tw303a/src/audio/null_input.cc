#include "null_input.h"

#include <cstring>

namespace audio {

NullInput::NullInput() {
    config_.sampleRate = 48000;
    config_.channels = 2;
    config_.bufferFrames = 1024;
    config_.sampleType = twSampleType::Float32;
}

NullInput::~NullInput() {}

int NullInput::openDevice(const std::string &deviceId, std::uint32_t preferredRate) {
    if (preferredRate > 0) {
        config_.sampleRate = preferredRate;
    }
    return 0;
}

int NullInput::closeDevice() {
    return 0;
}

int NullInput::startCapture() {
    return 0;
}

int NullInput::stopCapture() {
    return 0;
}

std::int32_t NullInput::read(float *interleaved, std::size_t frameCount) {
    // Return silence
    if (interleaved) {
        std::memset(interleaved, 0, frameCount * config_.channels * sizeof(float));
    }
    return static_cast<std::int32_t>(frameCount);
}

const AudioInputConfig &NullInput::getConfig() const {
    return config_;
}

std::vector<AudioInputDeviceInfo> NullInput::listDevices() const {
    return {{"null", "No Audio Input (Null)", 2}};
}

const char *NullInput::errorMessage() const {
    return lastError_.c_str();
}

}  // namespace audio
