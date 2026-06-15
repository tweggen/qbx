#ifndef _WASAPI_INPUT_H_
#define _WASAPI_INPUT_H_

#include "audio/audio_input.h"

#include <mutex>

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

namespace audio {

// Explicit capture lifecycle. Each public method is a guarded transition, so the
// legal order is readable from the code rather than inferred from a loose
// isCapturing_ flag plus null-checks:
//
//   Closed --openDevice--> Open        (failure: --> Closed)
//   Open   --startCapture--> Capturing (failure: stays Open)
//   Capturing --stopCapture--> Open
//   {Open,Capturing} --closeDevice--> Closed
enum class WasapiInputState {
    Closed,
    Open,
    Capturing,
};

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
    // Helpers that assume the caller already holds mutex_, so one transition can
    // compose another (closeDevice stops capture; openDevice's failure path
    // closes) without recursively re-locking.
    int  stopCaptureLocked_();
    void closeDeviceLocked_();

    AudioInputConfig config_;
    std::string lastError_;

    IMMDeviceEnumerator *enumerator_ = nullptr;
    IMMDevice *inputDevice_ = nullptr;
    IAudioClient *audioClient_ = nullptr;
    IAudioCaptureClient *captureClient_ = nullptr;

    bool comInitialized_ = false;

    // --- Threading model -----------------------------------------------------
    // This object has no thread of its own. The whole lifecycle (openDevice →
    // startCapture → read in a loop → stopCapture → closeDevice) is driven by the
    // single RecordingSession worker thread, and COM is initialised/uninitialised
    // on that same thread (per-thread apartment), so the calls are naturally
    // serialised. mutex_ guards state_ and every handle/field below so the object
    // is nonetheless safe if a control call (e.g. listDevices from the UI thread,
    // or a stop racing the read loop) ever arrives concurrently — the crashes
    // this replaces were torn-down handles being touched from two sides.
    WasapiInputState state_ = WasapiInputState::Closed;  // guarded by mutex_
    mutable std::mutex mutex_;
};

}  // namespace audio

#endif
