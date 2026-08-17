#ifndef _WASAPI_INPUT_H_
#define _WASAPI_INPUT_H_

#include "tw/devices/audio_input.h"
#include "tw/devices/audio_ring.h"

#include <atomic>
#include <mutex>
#include <thread>

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
    AudioInputStats stats() const override;
    std::vector<AudioInputDeviceInfo> listDevices() const override;
    const char *errorMessage() const override;
    const char *backendName() const override { return "wasapi"; }

private:
    // Helpers that assume the caller already holds mutex_, so one transition can
    // compose another (closeDevice stops capture; openDevice's failure path
    // closes) without recursively re-locking.
    int  stopCaptureLocked_();
    void closeDeviceLocked_();

    // The capture thread's whole life (proposal 21 L0): wait on the client's
    // event, drain every packet WHOLE into the ring, repeat.
    void captureThreadMain_();

    AudioInputConfig config_;
    std::string lastError_;

    IMMDeviceEnumerator *enumerator_ = nullptr;
    IMMDevice *inputDevice_ = nullptr;
    IAudioClient *audioClient_ = nullptr;
    IAudioCaptureClient *captureClient_ = nullptr;

    bool comInitialized_ = false;

    // --- Threading model -----------------------------------------------------
    // The CONTROL PLANE (openDevice → startCapture → stopCapture → closeDevice)
    // is driven by one thread — RecordingSession's worker — and COM is
    // initialised/uninitialised on that same thread (per-thread apartment), so
    // those calls are naturally serialised. mutex_ guards state_ and every
    // handle below so the object is nonetheless safe if a control call (e.g.
    // listDevices from the UI thread) arrives concurrently — the crashes this
    // replaces were torn-down handles being touched from two sides.
    //
    // Proposal 21 L0 adds ONE thread of our own: the capture thread. It is
    // started by startCapture() and joined by stopCapture(), and between those
    // two points it is the only user of captureClient_ and the ring's only
    // producer. It deliberately takes NO lock — stopCapture() holds mutex_
    // while it joins, so a capture thread that wanted mutex_ would deadlock the
    // stop. What makes that safe is the ownership split: the client handles are
    // created before the thread starts and released after it has been joined.
    //
    // read() is now a RING POP and takes no lock either. It is called by the
    // consumer thread (RecordingSession's worker today), which is the same
    // thread that drives the control plane, so it cannot race a close.
    WasapiInputState state_ = WasapiInputState::Closed;  // guarded by mutex_
    mutable std::mutex mutex_;

    void *captureEvent_ = nullptr;      // HANDLE; auto-reset, set by WASAPI
    std::thread captureThread_;
    std::atomic<bool> captureRun_{ false };
    std::atomic<std::uint64_t> wakeups_{ 0 };
    AudioRing ring_;
};

}  // namespace audio

#endif
