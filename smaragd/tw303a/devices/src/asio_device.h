// asio_device — the one shared IASIO instance, and the registry that hands it
// out. Proposal 35, Phase 2 (output half; the input half lands in Phase 3).
//
// PRIVATE header (devices/src/, like wasapi_input.h). It names no SDK type:
// `IASIO` is forward-declared and every ASIO enum crossing this boundary is
// one of ours, so including it does not require the SDK to be present.
//
// WHY ONE DEVICE, SHARED
//
// ASIO callbacks carry NO context pointer — `bufferSwitch(long index, ...)`
// and nothing else — so a process-global "the active device" pointer is not a
// shortcut, it is the only expressible shape. Two live drivers in one process
// would have to share that single pointer, so the second one silently steals
// the first one's callbacks. The registry makes the restriction explicit and
// answers the second caller with a readable error instead.
//
// It is also what makes FULL DUPLEX work: `AsioBackend` (playback) and
// `AsioInput` (recording, Phase 3) are thin facades over ONE `AsioDevice`, so
// starting a recording while ASIO playback runs does not restart the driver —
// see the start REFCOUNT below.
//
// THE OUTPUT WIDTH IS TWO. Decided with the requester on 2026-08-18 and
// recorded in proposal 35 § "Phase 2, re-planned": we create buffers for
// output channels 0 and 1 only (fewer if the driver has fewer) and report that
// number as `AudioConfig::channels`, never the driver's own output count. It
// is the shipped device rule — monitoring is stereo (`twSpeaker`, proposal 36
// B5) — rather than an approximation of it, and reporting 8 on an 8-out
// interface would make `twmonitor::interleave`'s `c % 2` fan-out spray the
// monitor mix across every output pair, i.e. onto headphone amps and outboard
// sends. A wider project still RENDERS its full width to a file; it is
// monitored on the first two channels, exactly as it is on WASAPI.

#pragma once

#include "tw/devices/audio_backend.h"

#include "asio_convert.h"

#include "tw/devices/audio_ring.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct IASIO;
struct ASIOTime;
struct ASIOCallbacks;

namespace audio {

// One driver's static facts, filled at open and then read-only. Kept separate
// from AudioConfig because most of it has no AudioConfig field and because the
// facades and the tests want to see it without a device in hand.
struct AsioDeviceInfo {
    std::string name;            // the registry name, i.e. what the user picked
    std::string clsid;
    std::string driverName;      // what the driver calls itself
    long        driverVersion = 0;
    long        inputChannels  = 0;   // what the DRIVER has, not what we opened
    long        outputChannels = 0;
    long        bufMin = 0, bufMax = 0, bufPreferred = 0, bufGranularity = 0;
    std::vector<std::uint32_t> rates;
};

class AsioDevice {
public:
    ~AsioDevice();

    // Control plane. All of these are serialised by stateMutex_ and must be
    // called from a thread with a message pump (the Qt main thread) or from a
    // CoInitialized worker — drivers are in-proc COM servers written for one.
    //
    // `clsidOrName` is a registry driver NAME (matched case-insensitively as a
    // substring, as the probe does) or a CLSID in "{...}" form. Empty means
    // the first registered driver.
    static std::shared_ptr<AsioDevice> acquire(const std::string &clsidOrName,
                                               std::uint32_t preferredRate,
                                               std::string *errorOut);

    // The last failure, for a caller that wants to say why rather than just
    // that. Never cleared by a success — read it only on a failure path.
    static std::string lastError();

    const AsioDeviceInfo &info() const { return info_; }

    // What the OUTPUT half is configured as: channels is the opened width (at
    // most 2, see the header comment), sampleRate is the rate in force,
    // bufferFrames the driver's current buffer, outputLatencyFrames what
    // ASIOGetLatencies reported AFTER the rate was set and the buffers built.
    AudioConfig outputConfig() const;

    std::vector<std::uint32_t> supportedRates() const { return info_.rates; }
    std::vector<std::uint32_t> availableBufferSizes() const;
    // Only while the start refcount is 0 — a size change tears the buffers
    // down, and doing that under a running recording would lose the take.
    int setBufferSize(std::uint32_t frames);

    void setRenderCallback(RenderCallback cb);

    // --- the INPUT half (Phase 3) -------------------------------------------
    //
    // WHICH inputs are open is demand-driven and GROW-ONLY: see asio_channels.h
    // for the mask convention and why the stream is not compacted. The set can
    // only be changed while the driver is STOPPED — ASIO fixes it at
    // createBuffers time — so a request that arrives mid-stream is recorded and
    // applied at the next start rather than restarting the stream under
    // whoever is listening.
    //
    // Returns 0 when the request was already satisfied or has been applied,
    // 1 when it was DEFERRED to the next start, -1 on failure.
    int requestInputChannels(std::uint64_t mask);

    // The interleaved stream the ring carries: max(open channel) + 1, with
    // unopened channels silent, so bit n stays input n all the way up.
    std::uint32_t inputStreamChannels() const;
    std::uint32_t inputLatencyFrames() const;

    // ONE consumer. Returns frames.
    std::size_t readInput(float *interleaved, std::size_t frames);
    AudioRing   *inputRing() { return &inRing_; }

    // REFCOUNTED. The first caller starts the driver; the last one stops it.
    // A recording started while playback runs therefore does not restart the
    // stream, and a recording alone starts it with the output half emitting
    // silence (there is no render callback attached, and `bufferSwitch`
    // memsets in that case).
    int  startRef();
    int  stopRef();
    bool isRunning() const { return started_.load(std::memory_order_acquire) > 0; }

    // Counters the control plane logs from OUTSIDE the callback.
    std::uint64_t resetRequests() const { return resetRequests_.load(std::memory_order_relaxed); }
    std::uint64_t callbackCount() const { return callbacks_.load(std::memory_order_relaxed); }
    // Callbacks the driver delivered after the gate closed. Not an error —
    // some drivers simply do this — but worth reporting, because it is the
    // difference between "the fence held" and "there was nothing to hold".
    std::uint64_t lateCallbacks() const { return lateCallbacks_.load(std::memory_order_relaxed); }
    bool          rateChangedFlag() const { return rateChanged_.load(std::memory_order_relaxed); }

private:
    AsioDevice() = default;
    AsioDevice(const AsioDevice &) = delete;
    AsioDevice &operator=(const AsioDevice &) = delete;

    int  open_(const std::string &clsidOrName, std::uint32_t preferredRate, std::string *errorOut);
    void close_();
    // Builds BOTH halves: outputs 0..1 and whatever `inOpenMask_` names.
    int  createBuffers_(long frames);
    int  reopenForChannels_(std::uint64_t mask);
    void disposeBuffers_();
    // Close the callback gate, then spin until none is in flight. See the .cc:
    // the callback thread is DRIVER-owned, so there is nothing to join, and
    // this pair is what preserves the invariant every AudioBackend caller
    // relies on.
    void fenceCallbacks_();

    // The trampolines. Static because ASIO's callback struct is four plain C
    // function pointers; they resolve the instance through s_active_.
    //
    // The signatures must match `ASIOCallbacks` EXACTLY — a function-pointer
    // assignment does not convert, and MinGW rejects it outright rather than
    // silently building something that would corrupt the stack at the first
    // callback. `ASIOBool` is `long` and `ASIOSampleRate` is `double`, so only
    // ASIOTime needs naming: it is forward-declared above, which keeps this
    // header free of the SDK even though it now spells one SDK type.
    static void       cbBufferSwitch(long index, long directProcess);
    static ASIOTime  *cbBufferSwitchTimeInfo(ASIOTime *params, long index, long directProcess);
    static void       cbSampleRateDidChange(double rate);
    static long       cbAsioMessage(long selector, long value, void *msg, double *opt);

    // ONE BODY for both entry points. Which one a driver calls is the HOST's
    // choice (we answer kAsioSupportsTimeInfo), and a driver that honours the
    // answer never calls the other again — measured 357/0 vs 0/322 on a
    // US-16x08 via `asio_probe --no-timeinfo`. Implementing one of the two
    // would be a silent dead stream on half the drivers in the world.
    void render_(long index);

    IASIO         *driver_ = nullptr;
    AsioDeviceInfo info_;

    // THE DRIVER KEEPS THIS POINTER. `ASIOCreateBuffers` does not copy the
    // callback struct — it stores the address and dereferences it on every
    // single bufferSwitch, for the whole life of the stream. A local in
    // createBuffers_() therefore dies while the driver is still calling
    // through it, and the driver jumps to whatever now occupies that stack
    // slot. Measured: an immediate SIGSEGV on the DRIVER's thread at a stack
    // address, with a backtrace that is pure garbage because the "return
    // address" was never a return address.
    //
    // It is held by pointer rather than by value so this header stays free of
    // the SDK (the type is forward-declared above); ~AsioDevice is defined in
    // the .cc, where it is complete.
    std::unique_ptr<ASIOCallbacks> asioCallbacks_;

    // Opened OUTPUT channels only (at most 2). Phase 3 appends the inputs.
    struct ChannelBuf {
        void     *buffers[2] = {nullptr, nullptr};
        AsioType  type = AsioType::Unsupported;
    };
    std::vector<ChannelBuf> outBufs_;

    // INPUT. `inChannelNums_` are the DRIVER channel indices we opened, in the
    // order createBuffers was given them; `inBufs_` is parallel to it. The
    // interleaved stream is `inStreamChannels_` wide (max index + 1), so a
    // sample from driver channel c lands at interleaved slot c and every slot
    // nobody opened stays silent.
    std::vector<ChannelBuf> inBufs_;
    std::vector<int>        inChannelNums_;
    std::uint64_t           inOpenMask_    = 0;   // what IS open
    std::uint64_t           inWantMask_    = 0;   // what has been asked for
    std::uint32_t           inStreamChannels_ = 0;
    std::uint32_t           inputLatency_  = 0;
    // The capture ring. NOT a second SPSC implementation: `AudioRing` is
    // already lock-free (head/tail atomics, no mutex) and is already driven by
    // the WASAPI, ALSA and file capture threads — the same shape a driver
    // callback has. Its drop-NEWEST-and-count overflow rule is the one
    // devices/CONTRACT.md inv. 20 requires, because in SPSC the producer may
    // not move the consumer's index.
    AudioRing               inRing_;
    std::vector<float>      inScratch_;   // interleaved, filled in the callback

    long                    bufferFrames_ = 0;
    double                  sampleRate_   = 48000.0;
    std::uint32_t           outputLatency_ = 0;
    bool                    postOutputReady_ = false;
    bool                    buffersBuilt_ = false;

    // Interleaved float scratch handed to the render callback, sized at
    // createBuffers time. The callback thread reads and writes it and nothing
    // else touches it while the stream runs.
    std::vector<float> scratch_;

    // The callback reads this without a lock, so it is swapped only while the
    // stream is stopped (setRenderCallback asserts that by taking stateMutex_
    // and the start refcount being observed atomically).
    RenderCallback callback_;
    std::atomic<bool> callbackValid_{false};

    mutable std::mutex   stateMutex_;
    std::atomic<int>     started_{0};
    std::atomic<int>     inCallback_{0};
    // THE GATE. `stopOutput()` promises no callback is in flight OR
    // FORTHCOMING when it returns, and on a driver-owned thread the second
    // half cannot be delivered by waiting: nothing stops the driver from
    // entering the trampoline one more time after ASIOStop() returns, and this
    // one measurably does (256 frames late, every run). What CAN be
    // guaranteed is that a late callback touches NOTHING — it is turned away
    // at the top of render_() before the render callback, the scratch or the
    // driver buffers are looked at. Closed BEFORE ASIOStop, so the window is
    // shut before the last callback can start rather than after.
    std::atomic<bool>    acceptCallbacks_{false};
    std::atomic<std::uint64_t> lateCallbacks_{0};
    std::atomic<std::uint64_t> callbacks_{0};
    std::atomic<std::uint64_t> resetRequests_{0};
    std::atomic<bool>    rateChanged_{false};
    std::atomic<bool>    badIndex_{false};

    // THE process-wide active device. See the header comment: this is forced
    // by the ABI, not chosen.
    static AsioDevice *s_active_;
    static std::mutex  s_registryMutex_;
    static std::weak_ptr<AsioDevice> s_instance_;
    static std::string s_lastError_;

    friend class AsioDeviceDeleter;
};

}  // namespace audio
