// asio_backend — see asio_backend.h. Proposal 35, Phase 2.

#include "asio_backend.h"

#include "asio_driver_list.h"
#include "asio_id.h"

#include "tw/core/twlog.h"

namespace audio {

AsioBackend::~AsioBackend()
{
    closeDevice();
}

int AsioBackend::openDevice(const std::string &deviceName, std::uint32_t preferredRate)
{
    std::lock_guard<std::mutex> lk(mutex_);
    error_.clear();

    // The dispatcher strips the `asio:` prefix, but openDevice is also
    // reachable directly (tools, tests), so parse defensively: a prefixed id
    // arriving here must not be looked up verbatim as a driver name.
    const ParsedDeviceId p = parseDeviceId(deviceName);
    const std::string native =
        (p.kind == DeviceIdKind::Asio) ? p.native
        : (deviceName == "default" ? std::string() : deviceName);

    if (device_) {
        // A double open is a no-op with a warning, which is exactly what
        // WASAPIBackend does — and it is not reachable from the app:
        // `twSpeaker::setOutputDevice` only records the id ("takes effect on
        // next startOutput"), so a device switch always passes through a
        // close. Deliberately NOT "close and re-acquire the other driver":
        // that path has no way to be tested on a one-driver machine, and
        // silently diverging from the WASAPI sibling would be worse than
        // matching it.
        TW_LOGW("devices", "AsioBackend::openDevice: already open ('%s'); ignoring the "
                           "request for '%s'",
                device_->info().name.c_str(), deviceName.c_str());
        return 0;
    }

    device_ = AsioDevice::acquire(native, preferredRate, &error_);
    if (!device_) {
        if (error_.empty()) error_ = AsioDevice::lastError();
        TW_LOGE("devices", "AsioBackend::openDevice('%s') failed: %s",
                deviceName.c_str(), error_.c_str());
        return -1;
    }

    if (pendingCallback_) device_->setRenderCallback(pendingCallback_);
    return 0;
}

int AsioBackend::closeDevice()
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (!device_) return 0;
    if (startedHere_) {
        device_->stopRef();
        startedHere_ = false;
    }
    // Dropping the shared_ptr is the close: the DEVICE goes away when the last
    // facade lets go, which is what keeps a recording alive across a playback
    // stop on the same driver (Phase 3).
    device_.reset();
    return 0;
}

int AsioBackend::startOutput()
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (!device_) return -1;
    if (startedHere_) return 0;
    if (device_->startRef() != 0) return -1;
    startedHere_ = true;
    return 0;
}

int AsioBackend::stopOutput()
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (!device_ || !startedHere_) return 0;
    // AsioDevice::stopRef fences the callbacks when the refcount reaches zero,
    // which is what preserves the "no callback in flight or forthcoming after
    // return" invariant every AudioBackend caller relies on. There is no
    // thread to join: the callback thread belongs to the driver.
    device_->stopRef();
    startedHere_ = false;
    return 0;
}

bool AsioBackend::isRunning() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return device_ && startedHere_ && device_->isRunning();
}

void AsioBackend::setRenderCallback(RenderCallback cb)
{
    std::lock_guard<std::mutex> lk(mutex_);
    pendingCallback_ = cb;
    if (device_) device_->setRenderCallback(std::move(cb));
}

AudioConfig AsioBackend::getConfig() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (!device_) return AudioConfig{};
    return device_->outputConfig();
}

std::vector<std::uint32_t> AsioBackend::supportedRates() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (!device_) return {};
    return device_->supportedRates();
}

std::vector<std::uint32_t> AsioBackend::getAvailableBufferSizes() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (!device_) return {};
    return device_->availableBufferSizes();
}

int AsioBackend::setBufferSize(std::uint32_t frameCount)
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (!device_) return -1;
    return device_->setBufferSize(frameCount);
}

std::vector<AudioDeviceInfo> AsioBackend::enumerateDevices() const
{
    // REGISTRY ONLY — no driver is loaded to fill this list. Loading each one
    // would run ASIO4ALL-style splash panels and licence nags just to populate
    // a combo box, and on a machine with a driver another app is holding it
    // would fail for reasons that say nothing about whether the user can pick
    // it. `sampleRate` is therefore 0 ("unknown"), which is what the field
    // already means elsewhere.
    std::vector<AudioDeviceInfo> out;
    for (const AsioDriverEntry &e : scanAsioDrivers()) {
        AudioDeviceInfo d;
        d.id         = makeDeviceId(DeviceIdKind::Asio, e.clsid);
        d.name       = e.description.empty() ? e.name : e.description;
        d.sampleRate = 0;
        out.push_back(d);
    }
    return out;
}

}  // namespace audio
