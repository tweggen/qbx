#ifndef _ALSA_INPUT_H_
#define _ALSA_INPUT_H_

#include "tw/devices/audio_input.h"
#include "tw/devices/audio_ring.h"

#include <atomic>
#include <thread>

#include <alsa/asoundlib.h>

namespace audio {

class ALSAInput : public AudioInput {
public:
    ALSAInput();
    ~ALSAInput() override;

    int openDevice(const std::string &deviceId = "default",
                   std::uint32_t preferredRate = 0) override;
    int closeDevice() override;
    int startCapture() override;
    int stopCapture() override;
    std::int32_t read(float *interleaved, std::size_t frameCount) override;

    const AudioInputConfig &getConfig() const override;
    AudioInputStats stats() const override;
    std::vector<AudioInputDeviceInfo> listDevices() const override;
    const char *errorMessage() const override;
    const char *backendName() const override { return "alsa"; }

    // ALSA supports user-selectable buffer sizes. Returns list of common presets.
    std::vector<uint32_t> getAvailableBufferSizes() const override;
    // Change buffer size (must be stopped). Reconfigures hw params.
    int setBufferSize(uint32_t frameCount) override;

private:
    // Proposal 21 L0: the capture thread. snd_pcm_wait() is ALSA's own "a period
    // is ready" event, so this is the event-driven producer the WASAPI path got
    // — whole periods into the ring, read() a pop. UNVERIFIED: written and
    // reviewed on Windows, compiled and run nowhere in the L0 gate.
    void captureThreadMain_();

    AudioInputConfig config_;
    std::string lastError_;
    snd_pcm_t *pcmHandle_ = nullptr;
    bool isCapturing_ = false;

    AudioRing ring_;
    std::thread captureThread_;
    std::atomic<bool> captureRun_{ false };
    std::atomic<std::uint64_t> wakeups_{ 0 };
};

}  // namespace audio

#endif
