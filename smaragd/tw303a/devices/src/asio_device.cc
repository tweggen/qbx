// asio_device — see asio_device.h for why there is exactly one of these.
// Proposal 35, Phase 2.

#include "asio_device.h"

#include "asio_bufsize.h"
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

    std::vector<ASIOBufferInfo> infos((std::size_t) useCh);
    for (long i = 0; i < useCh; ++i) {
        infos[(std::size_t) i].isInput    = ASIOFalse;
        infos[(std::size_t) i].channelNum = i;
        infos[(std::size_t) i].buffers[0] = nullptr;
        infos[(std::size_t) i].buffers[1] = nullptr;
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

    const ASIOError r = driver_->createBuffers(infos.data(), useCh, frames, asioCallbacks_.get());
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
    if (driver_->getLatencies(&lIn, &lOut) == ASE_OK && lOut >= 0)
        outputLatency_ = (std::uint32_t) lOut;
    else
        outputLatency_ = 0;

    return 0;
}

void AsioDevice::disposeBuffers_()
{
    if (driver_ && buffersBuilt_) driver_->disposeBuffers();
    buffersBuilt_ = false;
    outBufs_.clear();
    scratch_.clear();
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

// --- start / stop -----------------------------------------------------------

int AsioDevice::startRef()
{
    std::lock_guard<std::mutex> lk(stateMutex_);
    if (!driver_ || !buffersBuilt_) return -1;

    if (started_.fetch_add(1, std::memory_order_acq_rel) == 0) {
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

    inCallback_.fetch_sub(1, std::memory_order_acq_rel);
}

}  // namespace audio
