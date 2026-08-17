#include "alsa_input.h"

#include "tw/core/twlog.h"

#include <cstring>
#include <sstream>
#include <vector>

namespace audio {

ALSAInput::ALSAInput() {
    config_.sampleRate = 48000;
    config_.channels = 2;
    config_.bufferFrames = 1024;
    config_.sampleType = twSampleType::Float32;
}

ALSAInput::~ALSAInput() {
    closeDevice();
}

int ALSAInput::openDevice(const std::string &deviceId, std::uint32_t preferredRate) {
    snd_pcm_hw_params_t *hwParams = nullptr;
    int err;

    const char *devName = (deviceId == "default") ? "default" : deviceId.c_str();

    err = snd_pcm_open(&pcmHandle_, devName, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        lastError_ = std::string("Failed to open PCM device: ") + snd_strerror(err);
        return -1;
    }

    snd_pcm_hw_params_malloc(&hwParams);
    err = snd_pcm_hw_params_any(pcmHandle_, hwParams);
    if (err < 0) {
        lastError_ = std::string("Failed to get PCM parameters: ") + snd_strerror(err);
        snd_pcm_close(pcmHandle_);
        pcmHandle_ = nullptr;
        return -1;
    }

    // Set access type to interleaved
    err = snd_pcm_hw_params_set_access(pcmHandle_, hwParams, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) {
        lastError_ = std::string("Failed to set access: ") + snd_strerror(err);
        snd_pcm_close(pcmHandle_);
        pcmHandle_ = nullptr;
        return -1;
    }

    // Set format to float32
    err = snd_pcm_hw_params_set_format(pcmHandle_, hwParams, SND_PCM_FORMAT_FLOAT);
    if (err < 0) {
        // Fallback to S16 if float not supported
        err = snd_pcm_hw_params_set_format(pcmHandle_, hwParams, SND_PCM_FORMAT_S16);
        if (err < 0) {
            lastError_ = std::string("Failed to set format: ") + snd_strerror(err);
            snd_pcm_close(pcmHandle_);
            pcmHandle_ = nullptr;
            return -1;
        }
        config_.sampleType = twSampleType::Int16;
    }

    // Set channels
    unsigned int channels = (config_.channels > 0) ? config_.channels : 2;
    err = snd_pcm_hw_params_set_channels(pcmHandle_, hwParams, channels);
    if (err < 0) {
        lastError_ = std::string("Failed to set channels: ") + snd_strerror(err);
        snd_pcm_close(pcmHandle_);
        pcmHandle_ = nullptr;
        return -1;
    }
    config_.channels = channels;

    // Set sample rate
    unsigned int rate = (preferredRate > 0) ? preferredRate : config_.sampleRate;
    err = snd_pcm_hw_params_set_rate_near(pcmHandle_, hwParams, &rate, 0);
    if (err < 0) {
        lastError_ = std::string("Failed to set rate: ") + snd_strerror(err);
        snd_pcm_close(pcmHandle_);
        pcmHandle_ = nullptr;
        return -1;
    }
    config_.sampleRate = rate;

    // Write parameters to device
    err = snd_pcm_hw_params(pcmHandle_, hwParams);
    if (err < 0) {
        lastError_ = std::string("Failed to write parameters: ") + snd_strerror(err);
        snd_pcm_close(pcmHandle_);
        pcmHandle_ = nullptr;
        return -1;
    }

    // Get the actual buffer size from hardware parameters for latency calculation.
    snd_pcm_uframes_t bufferFrames = 0;
    snd_pcm_hw_params_get_buffer_size(hwParams, &bufferFrames);
    config_.bufferFrames = static_cast<uint32_t>(bufferFrames);

    // Calculate input latency as the buffer size (device latency ~= buffer time).
    config_.inputLatencyFrames = static_cast<uint32_t>(bufferFrames);

    // The ring the capture thread produces into (proposal 21 L0).
    {
        std::size_t want = static_cast<std::size_t>(config_.bufferFrames) * 4;
        if (want < 16384) want = 16384;
        ring_.reset(config_.channels, want);
    }

    snd_pcm_hw_params_free(hwParams);
    return 0;
}

int ALSAInput::closeDevice() {
    stopCapture();

    if (pcmHandle_) {
        snd_pcm_close(pcmHandle_);
        pcmHandle_ = nullptr;
    }

    return 0;
}

int ALSAInput::startCapture() {
    if (!pcmHandle_) {
        lastError_ = "PCM device not open";
        return -1;
    }

    ring_.clear();
    ring_.resetStats();
    wakeups_.store(0, std::memory_order_relaxed);

    int err = snd_pcm_start(pcmHandle_);
    if (err < 0) {
        lastError_ = std::string("Failed to start PCM: ") + snd_strerror(err);
        return -1;
    }

    captureRun_.store(true, std::memory_order_release);
    captureThread_ = std::thread([this] { captureThreadMain_(); });

    isCapturing_ = true;
    return 0;
}

// The capture thread. snd_pcm_wait() blocks until a period is ready, with a
// timeout so a stop is never more than 100 ms away; everything available is
// then read WHOLE into the ring. read() no longer touches the PCM handle, which
// is what lets one consumer poll at its own block size without losing frames.
void ALSAInput::captureThreadMain_() {
    tw::TwLog::markNonBlocking();
    tw::TwLog::nameThread( "audio-in-alsa" );

    const std::size_t chunkFrames = 1024;
    const std::uint32_t ch = config_.channels;
    const bool s16 = (config_.sampleType == twSampleType::Int16);

    std::vector<float> scratch(chunkFrames * ch, 0.0f);
    std::vector<std::int16_t> raw16(s16 ? chunkFrames * ch : 0);

    while (captureRun_.load(std::memory_order_acquire)) {
        const int r = snd_pcm_wait(pcmHandle_, 100);
        if (!captureRun_.load(std::memory_order_acquire)) break;
        if (r == 0) continue;                       // timeout: nothing ready
        if (r < 0) {
            snd_pcm_recover(pcmHandle_, r, 1);
            continue;
        }

        for (;;) {
            snd_pcm_sframes_t avail = snd_pcm_avail_update(pcmHandle_);
            if (avail < 0) { snd_pcm_recover(pcmHandle_, (int) avail, 1); break; }
            if (avail == 0) break;

            std::size_t want = (std::size_t) avail;
            if (want > chunkFrames) want = chunkFrames;

            snd_pcm_sframes_t got;
            if (s16) {
                got = snd_pcm_readi(pcmHandle_, raw16.data(), want);
                if (got > 0)
                    for (std::size_t i = 0; i < (std::size_t) got * ch; ++i)
                        scratch[i] = (float) raw16[i] / 32768.0f;
            } else {
                got = snd_pcm_readi(pcmHandle_, scratch.data(), want);
            }

            if (got < 0) { snd_pcm_recover(pcmHandle_, (int) got, 1); break; }
            if (got == 0) break;

            ring_.push(scratch.data(), (std::size_t) got);
            wakeups_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

int ALSAInput::stopCapture() {
    if (!pcmHandle_ || !isCapturing_) {
        return 0;
    }

    // The thread first (it owns the PCM handle while running), then the drop.
    captureRun_.store(false, std::memory_order_release);
    if (captureThread_.joinable()) captureThread_.join();

    snd_pcm_drop(pcmHandle_);
    isCapturing_ = false;
    return 0;
}

std::vector<uint32_t> ALSAInput::getAvailableBufferSizes() const {
    // Return a set of common buffer sizes for user selection.
    // These are standard power-of-2 sizes for audio work.
    return { 256, 512, 1024, 2048, 4096, 8192 };
}

int ALSAInput::setBufferSize(uint32_t frameCount) {
    // Can only change buffer size when capture is not running.
    if (isCapturing_ || !pcmHandle_) return -1;

    snd_pcm_hw_params_t *hwParams = nullptr;
    int err;

    snd_pcm_hw_params_malloc(&hwParams);
    err = snd_pcm_hw_params_any(pcmHandle_, hwParams);
    if (err < 0) {
        lastError_ = std::string("Failed to get PCM parameters: ") + snd_strerror(err);
        snd_pcm_hw_params_free(hwParams);
        return -1;
    }

    snd_pcm_uframes_t bufFrames = frameCount;
    err = snd_pcm_hw_params_set_buffer_size_near(pcmHandle_, hwParams, &bufFrames);
    if (err < 0) {
        lastError_ = std::string("Failed to set buffer size: ") + snd_strerror(err);
        snd_pcm_hw_params_free(hwParams);
        return -1;
    }

    err = snd_pcm_hw_params(pcmHandle_, hwParams);
    if (err < 0) {
        lastError_ = std::string("Failed to apply PCM parameters: ") + snd_strerror(err);
        snd_pcm_hw_params_free(hwParams);
        return -1;
    }

    // Update config with the actual buffer size (may differ from requested)
    config_.bufferFrames = static_cast<uint32_t>(bufFrames);
    config_.inputLatencyFrames = static_cast<uint32_t>(bufFrames);

    snd_pcm_hw_params_free(hwParams);
    return 0;
}

std::int32_t ALSAInput::read(float *interleaved, std::size_t frameCount) {
    // A ring pop (proposal 21 L0): the capture thread owns snd_pcm_readi.
    if (!interleaved) return -1;
    return static_cast<std::int32_t>(ring_.pop(interleaved, frameCount));
}

AudioInputStats ALSAInput::stats() const {
    AudioInputStats s;
    s.framesPushed = ring_.framesPushed();
    s.framesPopped = ring_.framesPopped();
    s.overrunFrames = ring_.overrunFrames();
    s.underrunFrames = ring_.underrunFrames();
    s.captureWakeups = wakeups_.load(std::memory_order_relaxed);
    return s;
}

const AudioInputConfig &ALSAInput::getConfig() const {
    return config_;
}

std::vector<AudioInputDeviceInfo> ALSAInput::listDevices() const {
    std::vector<AudioInputDeviceInfo> devices;

    // Add default device
    devices.push_back({"default", "Default Input", config_.channels});

    // TODO: Enumerate ALSA devices using snd_card_next, snd_ctl_card_info, etc.
    // For now, just return the default

    return devices;
}

const char *ALSAInput::errorMessage() const {
    return lastError_.c_str();
}

}  // namespace audio
