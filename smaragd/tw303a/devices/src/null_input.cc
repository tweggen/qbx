#include "null_input.h"

#include "tw/devices/midi_out_scheduler.h"

#include <algorithm>
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
    // Set a reasonable default latency estimate (one buffer frame).
    config_.inputLatencyFrames = config_.bufferFrames;
    return 0;
}

int NullInput::closeDevice() {
    return 0;
}

int NullInput::startCapture() {
    startNs_ = MidiOutScheduler::hostNowNs();
    producedFrames_ = 0;
    capturing_.store( true, std::memory_order_release );
    return 0;
}

int NullInput::stopCapture() {
    capturing_.store( false, std::memory_order_release );
    return 0;
}

std::int32_t NullInput::read(float *interleaved, std::size_t frameCount) {
    // A silent DEVICE: only the frames real time has produced since
    // startCapture(), never an unbounded stream. Nothing before startCapture().
    if (!capturing_.load(std::memory_order_acquire)) return 0;
    const std::int64_t now = MidiOutScheduler::hostNowNs();
    const std::uint64_t due =
        (std::uint64_t) ((double) (now - startNs_) * (double) config_.sampleRate / 1e9);
    if (due <= producedFrames_) return 0;
    const std::size_t n = std::min<std::size_t>(frameCount, (std::size_t) (due - producedFrames_));
    if (interleaved) {
        std::memset(interleaved, 0, n * config_.channels * sizeof(float));
    }
    producedFrames_ += n;
    return static_cast<std::int32_t>(n);
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
