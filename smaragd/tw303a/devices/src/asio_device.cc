// asio_device — see asio_device.h for why there is exactly one of these.
// Proposal 35, Phase 2.

#include "asio_device.h"

#include "asio_bufsize.h"
#include "asio_channels.h"
#include "asio_driver_list.h"

#include "tw/core/twlog.h"

#include <windows.h>
#include <combaseapi.h>

// SDK headers, types only — we compile ZERO SDK sources (proposal 35, "The
// MinGW ABI decision"). Driver discovery is our own registry scan, which is
// what lets us skip the MSVC-isms in host/pc/asiolist.cpp.
#include "iasiodrv.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#if !defined(_WIN64)
#  error "The ASIO backend is x64-only: the MinGW<->MSVC vtable bet holds because x64 has a single calling convention (proposal 35)."
#endif

namespace audio {

namespace {

// The rates worth probing with ASIOCanSampleRate. Measured on a US-16x08:
// 44100/48000/88200/96000 and nothing else, so the sweep really does narrow.
constexpr double kRateTable[] = {32000.0,  44100.0,  48000.0, 88200.0,
                                 96000.0, 176400.0, 192000.0};

AsioType mapType(ASIOSampleType t)
{
    switch (t) {
    case ASIOSTInt16LSB:   return AsioType::Int16LSB;
    case ASIOSTInt24LSB:   return AsioType::Int24LSB;
    case ASIOSTInt32LSB:   return AsioType::Int32LSB;
    case ASIOSTFloat32LSB: return AsioType::Float32LSB;
    case ASIOSTFloat64LSB: return AsioType::Float64LSB;
    default:               return AsioType::Unsupported;  // MSB and DSD: refused
    }
}

std::string lowerCopy(const std::string &s)
{
    std::string o;
    o.reserve(s.size());
    for (char c : s) o.push_back((char) std::tolower((unsigned char) c));
    return o;
}

}  // namespace

AsioDevice      *AsioDevice::s_active_ = nullptr;
std::mutex       AsioDevice::s_registryMutex_;
std::weak_ptr<AsioDevice> AsioDevice::s_instance_;
std::string      AsioDevice::s_lastError_;

// --- registry ---------------------------------------------------------------

std::string AsioDevice::lastError()
{
    std::lock_guard<std::mutex> lk(s_registryMutex_);
    return s_lastError_;
}

std::shared_ptr<AsioDevice> AsioDevice::acquire(const std::string &clsidOrName,
                                                std::uint32_t preferredRate,
                                                std::string *errorOut)
{
    std::lock_guard<std::mutex> lk(s_registryMutex_);

    if (std::shared_ptr<AsioDevice> live = s_instance_.lock()) {
        // Already have one. Same driver -> share it, which is what makes
        // record-while-playing work on one driver instance. Different driver
        // -> a readable refusal, because the callbacks have one global slot
        // and the second driver would silently steal the first one's.
        const std::string want = lowerCopy(clsidOrName);
        const bool same = clsidOrName.empty() ||
                          lowerCopy(live->info_.clsid) == want ||
                          lowerCopy(live->info_.name).find(want) != std::string::npos;
        if (same) return live;

        s_lastError_ = "another ASIO driver (" + live->info_.name +
                       ") is already in use; only one ASIO driver can be open at a time";
        if (errorOut) *errorOut = s_lastError_;
        TW_LOGW("devices", "AsioDevice: refusing '%s' — '%s' is already open",
                clsidOrName.c_str(), live->info_.name.c_str());
        return nullptr;
    }

    // `new` + a custom shared_ptr rather than make_shared: the constructor is
    // private and the destructor has to run under the registry's rules.
    std::shared_ptr<AsioDevice> dev(new AsioDevice());
    std::string err;
    if (dev->open_(clsidOrName, preferredRate, &err) != 0) {
        s_lastError_ = err;
        if (errorOut) *errorOut = err;
        return nullptr;
    }
    s_instance_ = dev;
    s_lastError_.clear();
    return dev;
}

// --- open / close -----------------------------------------------------------

int AsioDevice::open_(const std::string &clsidOrName, std::uint32_t preferredRate,
                      std::string *errorOut)
{
    auto fail = [&](const std::string &m) {
        if (errorOut) *errorOut = m;
        TW_LOGE("devices", "AsioDevice::open: %s", m.c_str());
        if (driver_) { driver_->Release(); driver_ = nullptr; }
        return -1;
    };

    const std::vector<AsioDriverEntry> drivers = scanAsioDrivers();
    if (drivers.empty()) return fail("no ASIO drivers are registered on this machine");

    const AsioDriverEntry *pick = nullptr;
    if (clsidOrName.empty()) {
        pick = &drivers.front();
    } else {
        const std::string want = lowerCopy(clsidOrName);
        for (const AsioDriverEntry &e : drivers) {
            if (lowerCopy(e.clsid) == want) { pick = &e; break; }
        }
        if (!pick) {
            for (const AsioDriverEntry &e : drivers) {
                if (lowerCopy(e.name).find(want) != std::string::npos) { pick = &e; break; }
            }
        }
        if (!pick) return fail("no ASIO driver matches '" + clsidOrName + "'");
    }

    std::wstring w(pick->clsid.begin(), pick->clsid.end());
    CLSID clsid;
    if (CLSIDFromString(w.c_str(), &clsid) != NOERROR)
        return fail("driver '" + pick->name + "' has an unparsable CLSID " + pick->clsid);

    // ASIO's COM usage is idiosyncratic and this is the heart of it: the
    // driver's CLSID doubles as its IID.
    const HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid,
                                        (void **) &driver_);
    if (FAILED(hr) || !driver_) {
        driver_ = nullptr;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "0x%08lX", (unsigned long) hr);
        return fail("CoCreateInstance failed for '" + pick->name + "': " + buf);
    }

    // init() takes a window handle; drivers park their control panel on it and
    // some (ASIO4ALL) refuse a null. The desktop window is the conventional
    // stand-in for a windowless host.
    if (driver_->init(GetDesktopWindow()) != ASIOTrue) {
        char err[128] = {0};
        driver_->getErrorMessage(err);
        err[sizeof(err) - 1] = 0;
        return fail("init() failed for '" + pick->name + "': " +
                    (err[0] ? err : "(no error message)"));
    }

    info_.name  = pick->name;
    info_.clsid = pick->clsid;

    char dn[64] = {0};
    driver_->getDriverName(dn);
    dn[sizeof(dn) - 1] = 0;
    info_.driverName    = dn;
    info_.driverVersion = driver_->getDriverVersion();

    long nIn = 0, nOut = 0;
    if (driver_->getChannels(&nIn, &nOut) != ASE_OK || nIn < 0 || nOut < 0 ||
        nIn > 4096 || nOut > 4096) {
        // The same implausibility check the probe makes: garbage here is the
        // first tell of a vtable-layout mismatch, before anything crashes.
        return fail("'" + pick->name + "' reported implausible channel counts");
    }
    info_.inputChannels  = nIn;
    info_.outputChannels = nOut;
    if (nOut <= 0) return fail("'" + pick->name + "' reports no output channels");

    driver_->getBufferSize(&info_.bufMin, &info_.bufMax, &info_.bufPreferred,
                           &info_.bufGranularity);

    for (double cand : kRateTable)
        if (driver_->canSampleRate(cand) == ASE_OK)
            info_.rates.push_back((std::uint32_t) cand);

    // The rate is set BEFORE the buffers are built and before the latencies
    // are read — see createBuffers_ for why that ordering is load-bearing.
    ASIOSampleRate cur = 0;
    driver_->getSampleRate(&cur);
    sampleRate_ = (double) cur;
    if (preferredRate != 0 && driver_->canSampleRate((double) preferredRate) == ASE_OK) {
        if (driver_->setSampleRate((double) preferredRate) == ASE_OK) {
            sampleRate_ = (double) preferredRate;
        } else {
            TW_LOGW("devices", "AsioDevice: '%s' accepted %u Hz in canSampleRate but "
                               "refused setSampleRate; staying at %.0f",
                    info_.name.c_str(), preferredRate, sampleRate_);
        }
    } else if (preferredRate != 0) {
        // NOT an error: the negotiator asked, the driver cannot, and the
        // speaker's resampler covers the difference exactly as it does on
        // WASAPI. Worth one line, because a project at an unsupported rate is
        // giving up the one-clock property ASIO was chosen for.
        TW_LOGI("devices", "AsioDevice: '%s' cannot run at %u Hz; using %.0f Hz "
                           "(the speaker will resample)",
                info_.name.c_str(), preferredRate, sampleRate_);
    }

    if (createBuffers_(info_.bufPreferred) != 0)
        return fail("createBuffers failed for '" + pick->name + "'");

    TW_LOGI("devices",
            "AsioDevice: '%s' (%s v%ld) open — %ld in / %ld out available, %u out opened, "
            "%.0f Hz, %ld frames, out latency %u",
            info_.name.c_str(), info_.driverName.c_str(), info_.driverVersion,
            info_.inputChannels, info_.outputChannels, (unsigned) outBufs_.size(),
            sampleRate_, bufferFrames_, outputLatency_);
    return 0;
}

int AsioDevice::createBuffers_(long frames)
{
    if (!driver_) return -1;
    disposeBuffers_();

    if (frames <= 0) frames = info_.bufPreferred > 0 ? info_.bufPreferred : 512;

    // THE OUTPUT WIDTH IS TWO — see the header. Never info_.outputChannels.
    const long useCh = info_.outputChannels < 2 ? info_.outputChannels : 2;
    if (useCh <= 0) return -1;

    // THE INPUT SET IS DEMAND-DRIVEN and grow-only (asio_channels.h). Nobody
    // having asked yet means input 0 alone, which is what
    // SObject::DEFAULT_RECORDING_CHANNELS selects anyway — a pro interface's
    // sixteen inputs are not opened because it happens to have sixteen.
    if (inOpenMask_ == 0)
        inOpenMask_ = asioGrowMask(0, inWantMask_ ? inWantMask_ : kAsioDefaultInputMask,
                                   (int) info_.inputChannels);
    inChannelNums_ = asioChannelsFromMask(inOpenMask_, (int) info_.inputChannels);
    const long inCh = (long) inChannelNums_.size();

    std::vector<ASIOBufferInfo> infos((std::size_t) (useCh + inCh));
    for (long i = 0; i < useCh; ++i) {
        infos[(std::size_t) i].isInput    = ASIOFalse;
        infos[(std::size_t) i].channelNum = i;
        infos[(std::size_t) i].buffers[0] = nullptr;
        infos[(std::size_t) i].buffers[1] = nullptr;
    }
    for (long i = 0; i < inCh; ++i) {
        ASIOBufferInfo &bi = infos[(std::size_t) (useCh + i)];
        bi.isInput    = ASIOTrue;
        bi.channelNum = inChannelNums_[(std::size_t) i];
        bi.buffers[0] = nullptr;
        bi.buffers[1] = nullptr;
    }

    // MEMBER, never a local — the driver keeps this pointer for the life of
    // the stream. See the declaration in asio_device.h for what happens when
    // it does not outlive createBuffers().
    asioCallbacks_.reset(new ASIOCallbacks());
    asioCallbacks_->bufferSwitch         = &AsioDevice::cbBufferSwitch;
    asioCallbacks_->sampleRateDidChange  = &AsioDevice::cbSampleRateDidChange;
    asioCallbacks_->asioMessage          = &AsioDevice::cbAsioMessage;
    asioCallbacks_->bufferSwitchTimeInfo = &AsioDevice::cbBufferSwitchTimeInfo;

    // The trampolines resolve through s_active_, so it must be live BEFORE
    // createBuffers: a driver may probe or even fire a callback from inside
    // the call.
    s_active_ = this;

    const ASIOError r = driver_->createBuffers(infos.data(), useCh + inCh, frames,
                                               asioCallbacks_.get());
    if (r != ASE_OK) {
        s_active_ = nullptr;
        TW_LOGE("devices", "AsioDevice: createBuffers(%ld ch, %ld frames) failed (%d)",
                useCh, frames, (int) r);
        return -1;
    }

    outBufs_.resize((std::size_t) useCh);
    bool anyUnsupported = false;
    for (long i = 0; i < useCh; ++i) {
        outBufs_[(std::size_t) i].buffers[0] = infos[(std::size_t) i].buffers[0];
        outBufs_[(std::size_t) i].buffers[1] = infos[(std::size_t) i].buffers[1];

        ASIOChannelInfo ci;
        std::memset(&ci, 0, sizeof(ci));
        ci.channel = i;
        ci.isInput = ASIOFalse;
        if (driver_->getChannelInfo(&ci) == ASE_OK) {
            outBufs_[(std::size_t) i].type = mapType(ci.type);
        }
        if (!asioTypeSupported(outBufs_[(std::size_t) i].type)) {
            anyUnsupported = true;
            TW_LOGE("devices", "AsioDevice: output channel %ld uses an unsupported sample "
                               "type (MSB or DSD); the backend handles little-endian "
                               "int/float only",
                    i);
        }
    }
    if (anyUnsupported) {
        driver_->disposeBuffers();
        outBufs_.clear();
        s_active_ = nullptr;
        return -1;
    }

    // The input half, parallel to inChannelNums_.
    inBufs_.assign((std::size_t) inCh, ChannelBuf{});
    bool inUnsupported = false;
    for (long i = 0; i < inCh; ++i) {
        const ASIOBufferInfo &bi = infos[(std::size_t) (useCh + i)];
        inBufs_[(std::size_t) i].buffers[0] = bi.buffers[0];
        inBufs_[(std::size_t) i].buffers[1] = bi.buffers[1];

        ASIOChannelInfo ci;
        std::memset(&ci, 0, sizeof(ci));
        ci.channel = inChannelNums_[(std::size_t) i];
        ci.isInput = ASIOTrue;
        if (driver_->getChannelInfo(&ci) == ASE_OK)
            inBufs_[(std::size_t) i].type = mapType(ci.type);
        if (!asioTypeSupported(inBufs_[(std::size_t) i].type)) {
            inUnsupported = true;
            TW_LOGE("devices", "AsioDevice: input channel %d uses an unsupported sample "
                               "type (MSB or DSD)",
                    inChannelNums_[(std::size_t) i]);
        }
    }
    if (inUnsupported) {
        driver_->disposeBuffers();
        outBufs_.clear();
        inBufs_.clear();
        s_active_ = nullptr;
        return -1;
    }

    // The stream stays indexed by DRIVER channel — see asio_channels.h. A
    // compacted stream would silently redefine every recordingChannels_ mask
    // in the project.
    inStreamChannels_ = (std::uint32_t) asioStreamWidthFor(inOpenMask_,
                                                           (int) info_.inputChannels);
    if (inStreamChannels_ > 0) {
        // Two seconds of ring: a recording consumer polls on a timer, and the
        // cost of depth here is bytes rather than latency (the ring is drained
        // by position, never waited on).
        // reset(CHANNELS, FRAMES) — that order, and getting it backwards sizes
        // the ring at one frame of 96000 channels, which the driver thread
        // then walks straight off the end of.
        inRing_.reset(inStreamChannels_, (std::size_t) (sampleRate_ * 2.0));
        inScratch_.assign((std::size_t) frames * inStreamChannels_, 0.0f);
    }

    bufferFrames_    = frames;
    buffersBuilt_    = true;
    postOutputReady_ = driver_->outputReady() == ASE_OK;
    scratch_.assign((std::size_t) frames * outBufs_.size(), 0.0f);

    // LATENCY IS READ HERE, not at open, and again after any rate change.
    // Measured on a US-16x08: output latency 702 frames at 44100 and 735 at
    // 48000, so a value cached before setSampleRate is wrong by 33 frames
    // before anything interesting has happened — and it feeds
    // meterLatencyFrames(), hence every position the meters, the MIDI-out pump
    // and both recorders compensate with.
    long lIn = 0, lOut = 0;
    if (driver_->getLatencies(&lIn, &lOut) == ASE_OK) {
        outputLatency_ = lOut >= 0 ? (std::uint32_t) lOut : 0;
        inputLatency_  = lIn  >= 0 ? (std::uint32_t) lIn  : 0;
    } else {
        outputLatency_ = 0;
        inputLatency_  = 0;
    }

    return 0;
}

void AsioDevice::disposeBuffers_()
{
    if (driver_ && buffersBuilt_) driver_->disposeBuffers();
    buffersBuilt_ = false;
    outBufs_.clear();
    scratch_.clear();
    inBufs_.clear();
    inChannelNums_.clear();
    inScratch_.clear();
    // inOpenMask_ is NOT cleared: the set is grow-only across a re-open, which
    // is what makes arming a channel a second time free.
}

void AsioDevice::close_()
{
    if (started_.load(std::memory_order_acquire) > 0) {
        acceptCallbacks_.store(false, std::memory_order_release);
        if (driver_) driver_->stop();
        started_.store(0, std::memory_order_release);
        fenceCallbacks_();
    }
    // Belt and braces for teardown: the trampolines resolve through s_active_,
    // so clearing it makes a callback arriving during destruction a no-op even
    // if the gate were somehow open.
    acceptCallbacks_.store(false, std::memory_order_release);
    disposeBuffers_();
    if (s_active_ == this) s_active_ = nullptr;
    if (driver_) {
        driver_->Release();
        driver_ = nullptr;
    }
}

AsioDevice::~AsioDevice()
{
    std::lock_guard<std::mutex> lk(stateMutex_);
    close_();
}

// --- queries ----------------------------------------------------------------

AudioConfig AsioDevice::outputConfig() const
{
    std::lock_guard<std::mutex> lk(stateMutex_);
    AudioConfig c;
    c.sampleRate   = (std::uint32_t) (sampleRate_ + 0.5);
    c.channels     = (std::uint32_t) outBufs_.size();
    c.bufferFrames = (std::uint32_t) (bufferFrames_ > 0 ? bufferFrames_ : 0);
    c.periodFrames = c.bufferFrames;
    // The device wire is de-interleaved and may differ per channel, so there
    // is no single twSampleType that describes it. Float32 is what the
    // CALLBACK produces, which is what this field is read for.
    c.sampleType         = twSampleType::Float32;
    c.outputLatencyFrames = outputLatency_;
    return c;
}

std::vector<std::uint32_t> AsioDevice::availableBufferSizes() const
{
    std::lock_guard<std::mutex> lk(stateMutex_);
    return asioBufferSizeCandidates(info_.bufMin, info_.bufMax, info_.bufPreferred,
                                    info_.bufGranularity);
}

int AsioDevice::setBufferSize(std::uint32_t frames)
{
    std::lock_guard<std::mutex> lk(stateMutex_);
    if (!driver_) return -1;
    if (started_.load(std::memory_order_acquire) > 0) {
        // Refused rather than honoured-later: rebuilding buffers under a
        // running stream would drop the take a recording is in the middle of.
        TW_LOGW("devices", "AsioDevice: setBufferSize refused while the driver is running");
        return -1;
    }
    const std::uint32_t snapped = asioSnapBufferSize(frames, info_.bufMin, info_.bufMax,
                                                     info_.bufPreferred, info_.bufGranularity);
    if (snapped == 0) return -1;
    if ((long) snapped == bufferFrames_) return 0;
    return createBuffers_((long) snapped);
}

void AsioDevice::setRenderCallback(RenderCallback cb)
{
    std::lock_guard<std::mutex> lk(stateMutex_);
    // The callback thread reads callback_ without a lock, so attach/detach is
    // published through callbackValid_ and the stream must be stopped for a
    // SWAP. Attaching before the first start (the normal case) is safe by
    // construction.
    callbackValid_.store(false, std::memory_order_release);
    fenceCallbacks_();
    callback_ = std::move(cb);
    callbackValid_.store(callback_ != nullptr, std::memory_order_release);
}

// --- the driver's control panel (Phase 5) -----------------------------------

int AsioDevice::openControlPanel()
{
    // stateMutex_ IS held across the modal call, deliberately. It does not
    // touch the audio path — the callback never takes this lock — and holding
    // it is what stops a start/stop or a buffer-size change racing a panel the
    // user has open. The UI thread is blocked anyway: it is the one inside the
    // driver's window.
    std::lock_guard<std::mutex> lk(stateMutex_);
    if (!driver_) return -1;

    const long beforeMin = info_.bufMin, beforeMax = info_.bufMax;
    const long beforePref = info_.bufPreferred, beforeGran = info_.bufGranularity;
    const double beforeRate = sampleRate_;
    const std::uint64_t beforeResets = resetRequests_.load(std::memory_order_relaxed);

    const ASIOError r = driver_->controlPanel();
    if (r != ASE_OK && r != ASE_NotPresent) {
        TW_LOGW("devices", "AsioDevice: controlPanel() returned %d", (int) r);
        return -1;
    }
    if (r == ASE_NotPresent) {
        TW_LOGI("devices", "AsioDevice: '%s' has no control panel", info_.name.c_str());
        return -1;
    }

    // RE-READ. The whole point of the panel on a driver like the US-16x08 is
    // that the buffer size lives in there and nowhere else, so the numbers we
    // reported a moment ago may now be wrong.
    driver_->getBufferSize(&info_.bufMin, &info_.bufMax, &info_.bufPreferred,
                           &info_.bufGranularity);
    ASIOSampleRate cur = 0;
    if (driver_->getSampleRate(&cur) == ASE_OK && (double) cur > 0.0)
        sampleRate_ = (double) cur;

    const bool bufMoved = info_.bufMin != beforeMin || info_.bufMax != beforeMax ||
                          info_.bufPreferred != beforePref ||
                          info_.bufGranularity != beforeGran;
    const bool rateMoved = sampleRate_ != beforeRate;
    const std::uint64_t resets =
        resetRequests_.load(std::memory_order_relaxed) - beforeResets;

    if (bufMoved || rateMoved || resets) {
        // NOT applied here. Rebuilding the buffers needs the driver stopped,
        // and the user may be listening to something; this is the same
        // "takes effect on the next Play" contract a device change has.
        TW_LOGI("devices",
                "AsioDevice: control panel changed something — buffer %ld/%ld/%ld/%ld"
                " -> %ld/%ld/%ld/%ld, rate %.0f -> %.0f, %llu reset request(s). Applied on"
                " the next Play.",
                beforeMin, beforeMax, beforePref, beforeGran, info_.bufMin, info_.bufMax,
                info_.bufPreferred, info_.bufGranularity, beforeRate, sampleRate_,
                (unsigned long long) resets);
    }
    return 0;
}

// --- the input half ---------------------------------------------------------

std::uint32_t AsioDevice::inputStreamChannels() const
{
    std::lock_guard<std::mutex> lk(stateMutex_);
    return inStreamChannels_;
}

std::uint32_t AsioDevice::inputLatencyFrames() const
{
    std::lock_guard<std::mutex> lk(stateMutex_);
    return inputLatency_;
}

std::size_t AsioDevice::readInput(float *interleaved, std::size_t frames)
{
    // NO LOCK: the ring is the synchronisation, and taking stateMutex_ here
    // would put a control-plane lock between a recording consumer and the
    // driver thread. ONE consumer, as AudioRing requires.
    return inRing_.pop(interleaved, frames);
}

int AsioDevice::requestInputChannels(std::uint64_t mask)
{
    std::lock_guard<std::mutex> lk(stateMutex_);
    if (!driver_) return -1;

    inWantMask_ = asioGrowMask(inWantMask_, mask, (int) info_.inputChannels);
    if (asioMaskSatisfied(inOpenMask_, inWantMask_, (int) info_.inputChannels))
        return 0;  // already open — the common case once a session is warm

    if (started_.load(std::memory_order_acquire) > 0) {
        // DEFERRED. Changing the set needs disposeBuffers + createBuffers,
        // which needs the driver stopped, and stopping it here would punch a
        // hole in whatever is currently being monitored or played. Same model
        // as a device change: it takes effect on the next start.
        TW_LOGI("devices",
                "AsioDevice: input channels 0x%llx requested while running; open set is "
                "0x%llx. Applied at the next start (stopping the driver now would gap "
                "playback).",
                (unsigned long long) inWantMask_, (unsigned long long) inOpenMask_);
        return 1;
    }
    return reopenForChannels_(inWantMask_) == 0 ? 0 : -1;
}

int AsioDevice::reopenForChannels_(std::uint64_t mask)
{
    // Caller holds stateMutex_ and has established the driver is stopped.
    const std::uint64_t grown = asioGrowMask(inOpenMask_, mask, (int) info_.inputChannels);
    if (grown == inOpenMask_ && buffersBuilt_) return 0;
    inOpenMask_ = grown;

    const long frames = bufferFrames_ > 0 ? bufferFrames_ : info_.bufPreferred;
    const int rc = createBuffers_(frames);
    if (rc == 0)
        TW_LOGI("devices", "AsioDevice: input channel set is now 0x%llx (%u-wide stream)",
                (unsigned long long) inOpenMask_, inStreamChannels_);
    return rc;
}

// --- start / stop -----------------------------------------------------------

int AsioDevice::startRef()
{
    std::lock_guard<std::mutex> lk(stateMutex_);
    if (!driver_ || !buffersBuilt_) return -1;

    if (started_.fetch_add(1, std::memory_order_acq_rel) == 0) {
        // A channel request that arrived while the stream was running was
        // deferred to exactly here — the one moment the set can change without
        // interrupting anybody.
        if (!asioMaskSatisfied(inOpenMask_, inWantMask_, (int) info_.inputChannels)) {
            if (reopenForChannels_(inWantMask_) != 0) {
                started_.store(0, std::memory_order_release);
                TW_LOGE("devices", "AsioDevice: could not open the requested input "
                                   "channels at start");
                return -1;
            }
        }
        // Open the gate BEFORE start(): a driver may fire its first callback
        // from inside start() itself.
        acceptCallbacks_.store(true, std::memory_order_release);
        const ASIOError r = driver_->start();
        if (r != ASE_OK) {
            acceptCallbacks_.store(false, std::memory_order_release);
            started_.store(0, std::memory_order_release);
            TW_LOGE("devices", "AsioDevice: start() failed (%d)", (int) r);
            return -1;
        }
    }
    return 0;
}

int AsioDevice::stopRef()
{
    std::lock_guard<std::mutex> lk(stateMutex_);
    if (!driver_) return -1;

    const int prev = started_.load(std::memory_order_acquire);
    if (prev <= 0) return 0;
    if (started_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        // Gate first, THEN stop, then drain. Closing it after ASIOStop would
        // leave exactly the window this exists to shut.
        acceptCallbacks_.store(false, std::memory_order_release);
        driver_->stop();
        fenceCallbacks_();
        // LOAD, not exchange: the count is evidence, and a counter that is
        // consumed by the act of reporting it cannot be queried afterwards
        // by anything else (which is how a claim about it went into a
        // contract document unverified).
        const std::uint64_t late = lateCallbacks_.load(std::memory_order_relaxed);
        if (late)
            TW_LOGI("devices",
                    "AsioDevice: '%s' delivered %llu callback(s) after stop() returned; "
                    "they were turned away at the gate (this is a driver trait, not an error)",
                    info_.name.c_str(), (unsigned long long) late);
    }
    return 0;
}

void AsioDevice::fenceCallbacks_()
{
    // THE CALLBACK THREAD IS DRIVER-OWNED, so there is nothing to join and
    // `stopOutput()`'s contract ("no callback in flight or forthcoming after
    // return") cannot be met by a join. It is met in two parts, and BOTH are
    // needed:
    //
    //   FORTHCOMING — `acceptCallbacks_` is cleared before ASIOStop, so a
    //     callback that begins afterwards is turned away at the top of
    //     render_() having touched nothing. This is not hypothetical: the
    //     Tascam US-16x08 delivers exactly one 256-frame callback after stop()
    //     returns, on every run. Waiting cannot fix that — only refusing can.
    //   IN FLIGHT — the spin below, which bounds the wait by one callback.
    //
    // The spin is capped and logs rather than hanging: a stuck driver must not
    // take the UI thread with it.
    for (int i = 0; i < 2000; ++i) {
        if (inCallback_.load(std::memory_order_acquire) == 0) return;
        Sleep(1);
    }
    TW_LOGW("devices", "AsioDevice: a callback was still in flight 2 s after stop()");
}

// --- the callback -----------------------------------------------------------

void AsioDevice::cbBufferSwitch(long index, long /*directProcess*/)
{
    AsioDevice *d = s_active_;
    if (d) d->render_(index);
}

ASIOTime *AsioDevice::cbBufferSwitchTimeInfo(ASIOTime *params, long index,
                                             long /*directProcess*/)
{
    // The ASIOTime this carries is the driver's own sample position and system
    // time. Phase 2 does not consume it — the engine clock is driven from the
    // frames the speaker delivers — but proposal 21 L6 will want exactly this,
    // and it is free here.
    AsioDevice *d = s_active_;
    if (d) d->render_(index);
    return params;
}

void AsioDevice::cbSampleRateDidChange(double rate)
{
    AsioDevice *d = s_active_;
    if (!d) return;
    // LATCH ONLY. No logging, no locking, no reconfiguration from a driver
    // thread; the control plane reports it and the device reopens on the next
    // Play. Declared known debt in proposal 35 — there is no live
    // renegotiation.
    d->rateChanged_.store(true, std::memory_order_relaxed);
    (void) rate;
}

long AsioDevice::cbAsioMessage(long selector, long value, void * /*msg*/, double * /*opt*/)
{
    switch (selector) {
    case kAsioSelectorSupported:
        return (value == kAsioEngineVersion || value == kAsioResetRequest ||
                value == kAsioSupportsTimeInfo)
                   ? 1
                   : 0;
    case kAsioEngineVersion:
        return 2;
    case kAsioSupportsTimeInfo:
        // YES — and therefore bufferSwitchTimeInfo becomes the entry point a
        // conforming driver uses exclusively. Both trampolines land in
        // render_(), which is what makes that safe; see asio_device.h.
        return 1;
    case kAsioResetRequest:
        if (AsioDevice *d = s_active_)
            d->resetRequests_.fetch_add(1, std::memory_order_relaxed);
        return 1;
    default:
        return 0;
    }
}

void AsioDevice::render_(long index)
{
    // THE GATE, first and before anything else is touched. After
    // stopOutput() returns, the caller is entitled to tear down the engine the
    // render callback reaches into, so a late callback must not reach it —
    // see fenceCallbacks_().
    if (!acceptCallbacks_.load(std::memory_order_acquire)) {
        lateCallbacks_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (index != 0 && index != 1) {
        // A double-buffer index outside {0,1} means the ABI fell apart. Write
        // nothing; the control plane can see the flag.
        badIndex_.store(true, std::memory_order_relaxed);
        return;
    }

    inCallback_.fetch_add(1, std::memory_order_acq_rel);
    callbacks_.fetch_add(1, std::memory_order_relaxed);

    const std::size_t ch     = outBufs_.size();
    const std::size_t frames = (std::size_t) bufferFrames_;

    if (ch == 0 || frames == 0 || scratch_.size() < ch * frames) {
        inCallback_.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }

    std::size_t produced = 0;
    if (callbackValid_.load(std::memory_order_acquire) && callback_) {
        produced = callback_(scratch_.data(), frames, (std::uint32_t) ch);
        if (produced > frames) produced = frames;
    }
    // Not attached, or short: the rest is silence. This is also the path a
    // recording-only session takes — the driver runs, the output half emits
    // nothing.
    if (produced < frames)
        std::fill(scratch_.begin() + (std::ptrdiff_t) (produced * ch),
                  scratch_.begin() + (std::ptrdiff_t) (frames * ch), 0.0f);

    // Strided read, contiguous write: ASIO is de-interleaved and each channel
    // may carry its own sample type.
    for (std::size_t c = 0; c < ch; ++c) {
        void *dst = outBufs_[c].buffers[index];
        if (!dst) continue;
        asioFromFloat(dst, outBufs_[c].type, scratch_.data(), frames, ch, c);
    }

    if (postOutputReady_ && driver_) driver_->outputReady();

    // --- the INPUT half ------------------------------------------------------
    //
    // Order matters only for latency, not correctness: the output is written
    // first so the driver has it as early as possible, and the capture is
    // pushed afterwards. Nothing here allocates, locks or logs — the ring is
    // lock-free and its overflow is a counter (inv. 20), which is why an
    // overrun costs a number a test can read rather than a stall in a driver
    // callback.
    if (inStreamChannels_ > 0 && !inBufs_.empty() &&
        inScratch_.size() >= (std::size_t) inStreamChannels_ * frames) {
        // Unopened slots stay silent, so a mask bit nobody opened reads as
        // silence rather than as another channel's audio.
        std::fill(inScratch_.begin(),
                  inScratch_.begin() + (std::ptrdiff_t) (frames * inStreamChannels_), 0.0f);
        for (std::size_t i = 0; i < inBufs_.size(); ++i) {
            const void *src = inBufs_[i].buffers[index];
            if (!src) continue;
            const std::size_t slot = (std::size_t) inChannelNums_[i];
            if (slot >= inStreamChannels_) continue;
            asioToFloat(inScratch_.data(), inStreamChannels_, slot, src, inBufs_[i].type,
                        frames);
        }
        inRing_.push(inScratch_.data(), frames);
    }

    inCallback_.fetch_sub(1, std::memory_order_acq_rel);
}

}  // namespace audio
