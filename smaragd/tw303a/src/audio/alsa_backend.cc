#include "audio/alsa_backend.h"

#include "twconvert.h"
#include "twsyslog.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

namespace audio {

ALSABackend::ALSABackend() = default;

ALSABackend::~ALSABackend()
{
    if (running_) stopOutput();
    if (pcm_)    closeDevice();
}

int ALSABackend::openDevice(const std::string &deviceName,
                            std::uint32_t preferredRate)
{
    if (preferredRate != 0) config_.sampleRate = preferredRate;

    int rc = snd_pcm_open(&pcm_, deviceName.c_str(),
                          SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) {
        syslog(LOG_ERR, "ALSABackend::openDevice: snd_pcm_open(%s) failed: %s",
               deviceName.c_str(), snd_strerror(rc));
        pcm_ = nullptr;
        return rc;
    }

    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(pcm_, params);

    snd_pcm_hw_params_set_access(pcm_, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm_, params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(pcm_, params, config_.channels);

    unsigned int rate = config_.sampleRate;
    int          dir  = 0;
    snd_pcm_hw_params_set_rate_near(pcm_, params, &rate, &dir);
    config_.sampleRate = rate;
    config_.sampleType = twSampleType::Int16;  // device-native format (S16_LE)

    snd_pcm_uframes_t bufferFrames = config_.bufferFrames;
    snd_pcm_uframes_t periodFrames = config_.periodFrames;
    snd_pcm_hw_params_set_buffer_size_near(pcm_, params, &bufferFrames);
    snd_pcm_hw_params_set_period_size_near(pcm_, params, &periodFrames, nullptr);
    config_.bufferFrames = static_cast<uint32_t>(bufferFrames);
    config_.periodFrames = static_cast<uint32_t>(periodFrames);

    rc = snd_pcm_hw_params(pcm_, params);
    if (rc < 0) {
        syslog(LOG_ERR, "ALSABackend::openDevice: snd_pcm_hw_params failed: %s",
               snd_strerror(rc));
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
        return rc;
    }

    floatBuffer_.assign(config_.bufferFrames * config_.channels, 0.0f);
    shortBuffer_.assign(config_.bufferFrames * config_.channels, 0);

    syslog(LOG_INFO,
           "ALSABackend: opened '%s', %u Hz, %u ch, buffer=%u, period=%u",
           deviceName.c_str(), config_.sampleRate, config_.channels,
           config_.bufferFrames, config_.periodFrames);
    return 0;
}

int ALSABackend::closeDevice()
{
    if (pcm_) {
        snd_pcm_drain(pcm_);
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
    }
    return 0;
}

std::vector<std::uint32_t> ALSABackend::supportedRates() const
{
    // Probe the standard candidate set against the device's hw params. Done on
    // a short-lived handle so it works whether or not a stream is open.
    static const unsigned int kCandidates[] = { 44100, 48000, 88200, 96000,
                                                 176400, 192000 };
    std::vector<std::uint32_t> out;

    snd_pcm_t *probe = nullptr;
    if (snd_pcm_open(&probe, "default", SND_PCM_STREAM_PLAYBACK,
                     SND_PCM_NONBLOCK) < 0 || !probe)
        return out;

    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_alloca(&params);
    if (snd_pcm_hw_params_any(probe, params) >= 0) {
        for (unsigned int r : kCandidates) {
            if (snd_pcm_hw_params_test_rate(probe, params, r, 0) == 0)
                out.push_back(r);
        }
    }
    snd_pcm_close(probe);
    return out;
}

std::vector<AudioDeviceInfo> ALSABackend::enumerateDevices() const
{
    std::vector<AudioDeviceInfo> devices;
    // Always offer the system default first.
    devices.push_back({ "default", "System default" });

    int card = -1;
    while (snd_card_next(&card) == 0 && card >= 0) {
        char *cardName = nullptr;
        if (snd_card_get_name(card, &cardName) == 0 && cardName) {
            AudioDeviceInfo d;
            d.id   = "hw:" + std::to_string(card);
            d.name = cardName;
            devices.push_back(d);
            free(cardName);
        }
    }
    return devices;
}

int ALSABackend::startOutput()
{
    if (!pcm_ || running_) return 0;

    int rc = snd_pcm_prepare(pcm_);
    if (rc < 0) {
        syslog(LOG_ERR, "ALSABackend::startOutput: snd_pcm_prepare: %s",
               snd_strerror(rc));
        return rc;
    }

    // Pre-fill two periods so the ring buffer has data before we start.
    writeChunk_(2 * config_.periodFrames);

    rc = snd_async_add_pcm_handler(&asyncHandle_, pcm_,
                                   &ALSABackend::asyncCallbackStatic_, this);
    if (rc < 0) {
        syslog(LOG_ERR, "ALSABackend::startOutput: snd_async_add_pcm_handler: %s",
               snd_strerror(rc));
        return rc;
    }

    rc = snd_pcm_start(pcm_);
    if (rc < 0) {
        syslog(LOG_ERR, "ALSABackend::startOutput: snd_pcm_start: %s",
               snd_strerror(rc));
        snd_async_del_handler(asyncHandle_);
        asyncHandle_ = nullptr;
        return rc;
    }

    running_ = true;
    return 0;
}

int ALSABackend::stopOutput()
{
    if (!running_) return 0;
    if (pcm_)         snd_pcm_drop(pcm_);
    if (asyncHandle_) {
        snd_async_del_handler(asyncHandle_);
        asyncHandle_ = nullptr;
    }
    running_ = false;
    return 0;
}

void ALSABackend::asyncCallbackStatic_(snd_async_handler_t *handler)
{
    auto *self = static_cast<ALSABackend *>(
        snd_async_handler_get_callback_private(handler));
    if (self) self->asyncCallback_();
}

void ALSABackend::asyncCallback_()
{
    snd_pcm_sframes_t avail = snd_pcm_avail_update(pcm_);
    if (avail < 0) {
        if (avail == -EPIPE) {
            syslog(LOG_WARNING, "ALSABackend: xrun, recovering");
            snd_pcm_prepare(pcm_);
        }
        return;
    }
    writeChunk_(static_cast<snd_pcm_uframes_t>(avail));
}

std::size_t ALSABackend::pullSamples_()
{
    if (!callback_) {
        std::fill(floatBuffer_.begin(), floatBuffer_.end(), 0.0f);
        return config_.bufferFrames;
    }
    std::size_t framesProduced =
        callback_(floatBuffer_.data(), config_.bufferFrames, config_.channels);
    if (framesProduced < config_.bufferFrames) {
        std::fill(floatBuffer_.begin() + framesProduced * config_.channels,
                  floatBuffer_.end(), 0.0f);
    }
    return config_.bufferFrames;
}

void ALSABackend::writeChunk_(snd_pcm_uframes_t chunkSize)
{
    if (!pcm_ || chunkSize == 0) return;

    snd_pcm_uframes_t remaining = chunkSize;
    while (remaining > 0) {
        snd_pcm_uframes_t framesNow = std::min<snd_pcm_uframes_t>(
            remaining, config_.bufferFrames);

        pullSamples_();
        // Interleaved N-channel float → S16_LE, via the shared converter
        // (replaces the former hand-rolled clip loop).
        twFormat srcFmt;
        srcFmt.sampleType = twSampleType::Float32;
        srcFmt.channels   = static_cast<std::uint16_t>(config_.channels);
        twFormat dstFmt = srcFmt;
        dstFmt.sampleType = twSampleType::Int16;
        twConvertFrames(srcFmt, floatBuffer_.data(), dstFmt, shortBuffer_.data(),
                        static_cast<length_t>(framesNow));

        snd_pcm_sframes_t written =
            snd_pcm_writei(pcm_, shortBuffer_.data(), framesNow);
        if (written < 0) {
            if (written == -EPIPE) {
                syslog(LOG_WARNING, "ALSABackend: write xrun, recovering");
                snd_pcm_prepare(pcm_);
                continue;
            }
            syslog(LOG_ERR, "ALSABackend::writeChunk_: snd_pcm_writei: %s",
                   snd_strerror(static_cast<int>(written)));
            return;
        }
        if (written == 0) return;
        remaining -= static_cast<snd_pcm_uframes_t>(written);
    }
}

}  // namespace audio
