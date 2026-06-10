#ifndef _WASAPI_INPUT_H_
#define _WASAPI_INPUT_H_

#include "audio/audio_input.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

namespace audio {

class WASAPIInput : public AudioInput {
public:
    WASAPIInput();
    ~WASAPIInput() override;

    int openDevice(const std::string &deviceId = "default",
                   std::uint32_t preferredRate = 0) override;
    int closeDevice() override;
    int startCapture() override;
    int stopCapture() override;
    std::int32_t read(float *interleaved, std::size_t frameCount) override;

    const AudioInputConfig &getConfig() const override;
    std::vector<AudioInputDeviceInfo> listDevices() const override;
    const char *errorMessage() const override;

private:
    AudioInputConfig config_;
    std::string lastError_;

    IMMDeviceEnumerator *enumerator_ = nullptr;
    IMMDevice *inputDevice_ = nullptr;
    IAudioClient *audioClient_ = nullptr;
    IAudioCaptureClient *captureClient_ = nullptr;

    bool isCapturing_ = false;
};

}  // namespace audio

#endif
