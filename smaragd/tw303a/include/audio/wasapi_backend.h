#ifndef _AUDIO_WASAPI_BACKEND_H_
#define _AUDIO_WASAPI_BACKEND_H_

#include "audio/audio_backend.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

// Forward-declare COM interfaces so this header doesn't pull in <windows.h>
// everywhere — keeps Qt MOC happy and stops the engine headers from leaking
// Windows macros (NOMINMAX et al.) into every translation unit.
struct IMMDeviceEnumerator;
struct IMMDevice;
struct IAudioClient;
struct IAudioRenderClient;
typedef void *HANDLE;

namespace audio {

enum class WasapiSampleFormat {
    Unknown,
    Float32,
    Int16,
    Int32,
};

// Explicit lifecycle state. Every public lifecycle method is a transition between
// these and is guarded by stateMutex_, so the legal sequence is unambiguous from
// the code rather than inferred from which handles happen to be non-null:
//
//   Closed --openDevice--> Opening --> Open        (failure: --> Closed)
//   Open   --startOutput--> Starting --> Running    (failure: --> Open)
//   Running --stopOutput--> Stopping --> Open
//   {Open,Running} --closeDevice--> Closing --> Closed
//
// The transient states (Opening/Starting/Stopping/Closing) exist only for the
// duration of a call while stateMutex_ is held; an observer never sees them
// because the mutex serialises callers. They make the intent of each method
// self-documenting and turn a half-completed open into a clean Closed rather than
// a "handles partly set" limbo.
enum class WasapiState {
    Closed,
    Opening,
    Open,
    Starting,
    Running,
    Stopping,
    Closing,
};

class WASAPIBackend : public AudioBackend {
public:
    WASAPIBackend();
    ~WASAPIBackend() override;

    int  openDevice(const std::string &deviceName = "default",
                    std::uint32_t preferredRate = 0) override;
    int  closeDevice() override;
    int  startOutput() override;
    int  stopOutput() override;
    bool isRunning() const override;

    void setRenderCallback(RenderCallback cb) override;
    AudioConfig getConfig() const override;

    // Shared mode: the OS mixer owns the rate, so this is just the current mix
    // rate (known after openDevice; empty before). Exclusive-mode enumeration
    // is future work — see proposal 02.
    std::vector<std::uint32_t> supportedRates() const override;
    std::vector<AudioDeviceInfo> enumerateDevices() const override;

    const char *name() const override { return "wasapi"; }

private:
    void audioThreadProc_();
    int  renderOnce_();

    // The following helpers all assume the caller already holds stateMutex_; the
    // public methods are the locking wrappers. Keeping the logic in *_locked_
    // helpers lets one transition compose another (e.g. closeDevice stops a
    // running stream) without recursively re-locking.
    int  startOutputLocked_();
    int  stopOutputLocked_();
    // Release all device/COM handles and return to Closed.
    void releaseDevice_();

    IMMDeviceEnumerator *enumerator_   = nullptr;
    IMMDevice           *device_       = nullptr;
    IAudioClient        *audioClient_  = nullptr;
    IAudioRenderClient  *renderClient_ = nullptr;
    HANDLE               bufferReady_  = nullptr;

    WasapiSampleFormat   sampleFormat_ = WasapiSampleFormat::Unknown;
    AudioConfig          config_;
    RenderCallback       callback_;

    std::vector<float>   floatScratch_;  // interleaved float pulled from the synth

    // --- Threading model -----------------------------------------------------
    // Two threads touch this object:
    //   1. The CONTROL thread (the caller — twSpeaker on the UI thread): drives
    //      openDevice / startOutput / stopOutput / closeDevice and the queries.
    //   2. The AUDIO thread (thread_, owned here): runs audioThreadProc_.
    //
    // stateMutex_ serialises every control-thread lifecycle transition and guards
    // state_ together with all the device handles, config_, sampleFormat_,
    // callback_ and floatScratch_ above. The audio thread NEVER acquires it — that
    // would both add unbounded latency to the realtime path and deadlock the
    // thread_.join() that stopOutput performs while holding the lock. Instead the
    // audio thread only runs while state_ == Running, and everything it reads is
    // established before the thread is created (in startOutputLocked_) and not
    // released or mutated until after it is joined. The handshake that tells it to
    // exit is the stopFlag_ atomic plus a SetEvent on bufferReady_.
    std::thread          thread_;
    std::atomic<bool>    stopFlag_{false};
    WasapiState          state_ = WasapiState::Closed;  // guarded by stateMutex_
    mutable std::mutex   stateMutex_;

    bool                 comInitialized_ = false;
};

}  // namespace audio

#endif
