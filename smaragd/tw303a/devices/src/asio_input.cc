// asio_input — see asio_input.h. Proposal 35, Phase 3.

#include "asio_input.h"

#include "asio_channels.h"
#include "asio_driver_list.h"
#include "asio_id.h"

#include "tw/core/twlog.h"

namespace audio {

AsioInput::~AsioInput()
{
    closeDevice();
}

int AsioInput::openDevice(const std::string &deviceId, std::uint32_t preferredRate)
{
    std::lock_guard<std::mutex> lk(mutex_);
    error_.clear();

    const ParsedDeviceId p = parseDeviceId(deviceId);
    const std::string native =
        (p.kind == DeviceIdKind::Asio) ? p.native
        : (deviceId == "default" ? std::string() : deviceId);

    if (device_) {
        TW_LOGW("devices", "AsioInput::openDevice: already open ('%s'); ignoring the "
                           "request for '%s'",
                device_->info().name.c_str(), deviceId.c_str());
        return 0;
    }

    device_ = AsioDevice::acquire(native, preferredRate, &error_);
    if (!device_) {
        if (error_.empty()) error_ = AsioDevice::lastError();
        TW_LOGE("devices", "AsioInput::openDevice('%s') failed: %s", deviceId.c_str(),
                error_.c_str());
        return -1;
    }

    // A request made BEFORE the device existed is applied now, while the
    // driver is certainly stopped — which is the cheapest moment there is.
    if (pendingMask_) device_->requestInputChannels(pendingMask_);

    refreshConfig_();
    return 0;
}

int AsioInput::closeDevice()
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (!device_) return 0;
    if (startedHere_) {
        device_->stopRef();
        startedHere_ = false;
    }
    // Dropping the shared_ptr is the close. If PLAYBACK still holds the device
    // the driver stays open and running — which is the whole point of the
    // refcount: stopping a recording must not stop the music.
    device_.reset();
    return 0;
}

int AsioInput::startCapture()
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (!device_) return -1;
    if (startedHere_) return 0;
    if (device_->startRef() != 0) return -1;
    startedHere_ = true;
    // A deferred channel request is applied inside startRef, so the geometry
    // can have changed between open and here.
    refreshConfig_();
    return 0;
}

int AsioInput::stopCapture()
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (!device_ || !startedHere_) return 0;
    device_->stopRef();
    startedHere_ = false;
    return 0;
}

std::int32_t AsioInput::read(float *interleaved, std::size_t frameCount)
{
    // Deliberately NOT under mutex_: this is the recording consumer's hot path
    // and the ring is its own synchronisation. The shared_ptr is copied under
    // the lock so a concurrent closeDevice cannot free the device mid-read.
    std::shared_ptr<AsioDevice> dev;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        dev = device_;
    }
    if (!dev || !interleaved) return -1;
    return (std::int32_t) dev->readInput(interleaved, frameCount);
}

int AsioInput::requestChannels(std::uint64_t mask)
{
    std::lock_guard<std::mutex> lk(mutex_);
    pendingMask_ = pendingMask_ | mask;   // grow-only here too
    if (!device_) return 0;               // applied at openDevice
    const int rc = device_->requestInputChannels(mask);
    if (rc == 0) refreshConfig_();        // the stream width may have grown
    return rc;
}

void AsioInput::refreshConfig_()
{
    if (!device_) return;
    const AudioConfig out = device_->outputConfig();
    config_.sampleRate         = out.sampleRate;
    config_.bufferFrames       = out.bufferFrames;
    config_.sampleType         = twSampleType::Float32;
    config_.channels           = device_->inputStreamChannels();
    // What the DRIVER has, as against the demand-driven set we opened above.
    // The two differ on every ASIO device — nobody having asked yet means
    // input 0 alone (createBuffers_) — and a channel PICKER needs this one.
    config_.deviceChannels     = (std::uint32_t) device_->info().inputChannels;
    config_.inputLatencyFrames = device_->inputLatencyFrames();
}

const AudioInputConfig &AsioInput::getConfig() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return config_;
}

AudioInputStats AsioInput::stats() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    AudioInputStats s;
    if (!device_) return s;
    AudioRing *r = const_cast<AsioDevice *>(device_.get())->inputRing();
    s.framesPushed  = r->framesPushed();
    s.framesPopped  = r->framesPopped();
    s.overrunFrames = r->overrunFrames();
    s.underrunFrames = r->underrunFrames();
    // captureWakeups has no meaning here: there is no capture THREAD. The
    // driver's callback is the producer, and callbackCount() is its twin —
    // reported so a caller can still tell a silent device from a stopped one.
    s.captureWakeups = device_->callbackCount();
    return s;
}

std::vector<AudioInputDeviceInfo> AsioInput::listDevices() const
{
    // REGISTRY ONLY — no driver is loaded. Same reasoning as the output side:
    // instantiating every installed driver to fill a combo box runs splash
    // screens and licence nags, and fails for reasons that say nothing about
    // whether the user may pick it. `channels` is therefore 0 ("not known
    // without opening"), which is what proposal 35 specified for enumeration.
    std::vector<AudioInputDeviceInfo> out;
    for (const AsioDriverEntry &e : scanAsioDrivers()) {
        AudioInputDeviceInfo d;
        d.id         = makeDeviceId(DeviceIdKind::Asio, e.clsid);
        d.name       = e.description.empty() ? e.name : e.description;
        d.channels   = 0;
        d.sampleRate = 0;
        out.push_back(d);
    }
    return out;
}

const char *AsioInput::errorMessage() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return error_.c_str();
}

}  // namespace audio
