#include "tw/devices/alsa_backend.h"

#include "tw/core/twconvert.h"
#include "tw/core/twsyslog.h"

#include <algorithm>
#include <chrono>
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

    // Calculate output latency as the buffer size (device latency ~= buffer time).
    // In ALSA shared mode, actual latency also includes driver/OS buffering,
    // but buffer size is the main controllable component.
    config_.outputLatencyFrames = config_.bufferFrames;

    syslog(LOG_INFO,
           "ALSABackend: opened '%s', %u Hz, %u ch, buffer=%u, period=%u, latency=%u frames",
           deviceName.c_str(), config_.sampleRate, config_.channels,
           config_.bufferFrames, config_.periodFrames, config_.outputLatencyFrames);
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

    // A "card" is not a single PCM device -- 'hw:<card>' alone means
    // 'hw:<card>,0', which is a real device on some cards and a dead end on
    // others (an HDA card's HDMI outputs are commonly device 3 / device 7,
    // never 0; opening 'hw:1' on such a card fails with ENOENT). Walk each
    // card's own device list, exactly as `aplay -l` does, and filter to the
    // ones that actually offer a PLAYBACK substream.
    int card = -1;
    while (snd_card_next(&card) == 0 && card >= 0) {
        char *cardName = nullptr;
        if (snd_card_get_name(card, &cardName) != 0 || !cardName) continue;

        const std::string ctlName = "hw:" + std::to_string(card);
        snd_ctl_t *ctl = nullptr;
        if (snd_ctl_open(&ctl, ctlName.c_str(), 0) < 0) {
            syslog(LOG_WARNING,
                   "ALSABackend::enumerateDevices: snd_ctl_open(%s) failed, skipping card",
                   ctlName.c_str());
            free(cardName);
            continue;
        }

        int device = -1;
        while (snd_ctl_pcm_next_device(ctl, &device) == 0 && device >= 0) {
            snd_pcm_info_t *info;
            snd_pcm_info_alloca(&info);
            snd_pcm_info_set_device(info, device);
            snd_pcm_info_set_subdevice(info, 0);
            snd_pcm_info_set_stream(info, SND_PCM_STREAM_PLAYBACK);
            // No playback substream at this device index (e.g. a
            // capture-only device) -- not selectable as an output, skip it.
            if (snd_ctl_pcm_info(ctl, info) < 0) continue;

            AudioDeviceInfo d;
            d.id = "hw:" + std::to_string(card) + "," + std::to_string(device);
            d.name = cardName;
            const char *pcmName = snd_pcm_info_get_name(info);
            if (pcmName && *pcmName) {
                d.name += ", ";
                d.name += pcmName;
            }
            devices.push_back(d);
        }
        snd_ctl_close(ctl);
        free(cardName);
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

    // No explicit snd_pcm_start(): writing enough frames to reach the
    // device's start_threshold (default == buffer size for a playback
    // stream) starts it automatically, which is what ioThreadFunc_'s first
    // few writes do. That sidesteps a real ordering hazard the old prefill-
    // then-explicit-start sequence had -- see the ioThreadFunc_ comment.
    stopRequested_.store(false, std::memory_order_relaxed);
    running_ = true;
    ioThread_ = std::thread(&ALSABackend::ioThreadFunc_, this);
    return 0;
}

int ALSABackend::stopOutput()
{
    if (!running_) return 0;

    // Signal and join FIRST, so pcm_ is touched by exactly one thread at a
    // time -- the ALSA handle has no internal locking of its own, and
    // snd_pcm_drop() from the caller's thread while ioThreadFunc_ might still
    // be inside snd_pcm_writei() on the SAME handle is a real data race, not
    // a documented "safe to interrupt from another thread" pattern (that
    // pattern relies on the caller itself being the one blocked, e.g. from a
    // signal handler on its own stack). ioThreadFunc_ writes in small
    // (period-sized) chunks and checks stopRequested_ between them, so the
    // join is bounded by roughly one chunk's worth of blocking time.
    stopRequested_.store(true, std::memory_order_relaxed);
    if (ioThread_.joinable()) ioThread_.join();
    if (pcm_) snd_pcm_drop(pcm_);
    running_ = false;
    return 0;
}

std::vector<uint32_t> ALSABackend::getAvailableBufferSizes() const
{
    // Return a set of common buffer sizes for user selection.
    // These are standard power-of-2 sizes for audio work.
    return { 256, 512, 1024, 2048, 4096, 8192 };
}

int ALSABackend::setBufferSize(uint32_t frameCount)
{
    // Can only change buffer size when device is not running.
    if (running_ || !pcm_) return -1;

    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_alloca(&params);

    int rc = snd_pcm_hw_params_any(pcm_, params);
    if (rc < 0) {
        syslog(LOG_ERR, "ALSABackend::setBufferSize: snd_pcm_hw_params_any: %s",
               snd_strerror(rc));
        return -1;
    }

    snd_pcm_uframes_t bufFrames = frameCount;
    rc = snd_pcm_hw_params_set_buffer_size_near(pcm_, params, &bufFrames);
    if (rc < 0) {
        syslog(LOG_ERR, "ALSABackend::setBufferSize: snd_pcm_hw_params_set_buffer_size_near: %s",
               snd_strerror(rc));
        return -1;
    }

    rc = snd_pcm_hw_params(pcm_, params);
    if (rc < 0) {
        syslog(LOG_ERR, "ALSABackend::setBufferSize: snd_pcm_hw_params: %s",
               snd_strerror(rc));
        return -1;
    }

    // Update config with the actual buffer size (may differ from requested)
    config_.bufferFrames = static_cast<uint32_t>(bufFrames);
    config_.outputLatencyFrames = static_cast<uint32_t>(bufFrames);

    // Resize scratch buffers to match new buffer size
    floatBuffer_.assign(config_.bufferFrames * config_.channels, 0.0f);
    shortBuffer_.assign(config_.bufferFrames * config_.channels, 0);

    syslog(LOG_INFO,
           "ALSABackend::setBufferSize: changed to %u frames (requested %u)",
           (unsigned)config_.bufferFrames, (unsigned)frameCount);

    return 0;
}

// A stock desktop's "default" ALSA PCM does not name a hardware device at
// all -- on Ubuntu it resolves through pipewire-alsa's (or, before that,
// pulseaudio's) userspace PCM plugin, so the actual mixing/routing is owned
// by the session's sound server rather than the kernel driver. That plugin
// implements ordinary blocking/non-blocking I/O but NOT the RT-signal async
// notification snd_async_add_pcm_handler() needs: opening 'default' and
// calling it returned -ENOSYS ("Function not implemented") every time,
// so no audio was ever produced on a stock install, only on a raw hw:N
// device where a real kernel driver backs the signal.
//
// The fix is to not need RT signals at all: a small dedicated thread that
// blocks in snd_pcm_writei() is the standard, portable way to drive ALSA
// (it's what aplay(1) itself does), and it works identically over hw:,
// dmix, and the pipewire/pulse plugin.
//
// It also closes a second, independent bug this design's callback shape
// invited: pullSamples_() must request EXACTLY the number of frames about
// to be written to the device, never more. The render callback's `frames`
// argument is a CONSUME count -- the engine's playhead advances by exactly
// that many frames per call (twSpeaker::renderCallbackBody /
// AudioEngine::pullBlock) -- so a backend that pulls a full buffer's worth
// but only ever writes a smaller "available space" chunk of it (the old
// asyncCallback_ / avail_update shape) throws away already-consumed frames
// and skips the timeline forward on every single write. Chunking pull and
// write together, one-for-one, is what avoids that.
void ALSABackend::ioThreadFunc_()
{
    const std::size_t chunk = std::max<std::size_t>(
        1, std::min<std::size_t>(config_.periodFrames, config_.bufferFrames));

    while (!stopRequested_.load(std::memory_order_relaxed)) {
        pullSamples_(chunk);
        writeChunk_(chunk);
    }
}

std::size_t ALSABackend::pullSamples_(std::size_t framesWanted)
{
    if (!callback_) {
        std::fill_n(floatBuffer_.begin(), framesWanted * config_.channels, 0.0f);
        return framesWanted;
    }
    std::size_t framesProduced =
        callback_(floatBuffer_.data(), framesWanted, config_.channels);
    if (framesProduced < framesWanted) {
        std::fill(floatBuffer_.begin() + framesProduced * config_.channels,
                  floatBuffer_.begin() + framesWanted * config_.channels, 0.0f);
    }
    return framesWanted;
}

void ALSABackend::writeChunk_(std::size_t frameCount)
{
    if (!pcm_ || frameCount == 0) return;

    // Interleaved N-channel float -> S16_LE, via the shared converter.
    twFormat srcFmt;
    srcFmt.sampleType = twSampleType::Float32;
    srcFmt.channels   = static_cast<std::uint16_t>(config_.channels);
    twFormat dstFmt = srcFmt;
    dstFmt.sampleType = twSampleType::Int16;
    twConvertFrames(srcFmt, floatBuffer_.data(), dstFmt, shortBuffer_.data(),
                    static_cast<length_t>(frameCount));

    std::size_t offset = 0;
    while (offset < frameCount) {
        if (stopRequested_.load(std::memory_order_relaxed)) return;

        snd_pcm_sframes_t written = snd_pcm_writei(
            pcm_, shortBuffer_.data() + offset * config_.channels,
            frameCount - offset);
        if (written < 0) {
            if (written == -EPIPE) {
                syslog(LOG_WARNING, "ALSABackend: xrun, recovering");
                snd_pcm_prepare(pcm_);
                continue;
            }
            if (written == -EAGAIN) continue;
            syslog(LOG_ERR, "ALSABackend::writeChunk_: snd_pcm_writei: %s",
                   snd_strerror(static_cast<int>(written)));
            return;
        }
        if (written == 0) return;
        offset += static_cast<std::size_t>(written);
    }
}

}  // namespace audio
