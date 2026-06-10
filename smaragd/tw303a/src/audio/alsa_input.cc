#include "alsa_input.h"

#include <cstring>
#include <sstream>

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

    int err = snd_pcm_start(pcmHandle_);
    if (err < 0) {
        lastError_ = std::string("Failed to start PCM: ") + snd_strerror(err);
        return -1;
    }

    isCapturing_ = true;
    return 0;
}

int ALSAInput::stopCapture() {
    if (!pcmHandle_ || !isCapturing_) {
        return 0;
    }

    snd_pcm_drop(pcmHandle_);
    isCapturing_ = false;
    return 0;
}

std::int32_t ALSAInput::read(float *interleaved, std::size_t frameCount) {
    if (!pcmHandle_) {
        return -1;
    }

    snd_pcm_sframes_t frames = snd_pcm_readi(pcmHandle_, interleaved, frameCount);

    if (frames < 0) {
        // Handle underrun
        if (frames == -EPIPE) {
            snd_pcm_recover(pcmHandle_, frames, 1);
            return 0;
        }
        lastError_ = std::string("Read error: ") + snd_strerror(frames);
        return -1;
    }

    return static_cast<std::int32_t>(frames);
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
