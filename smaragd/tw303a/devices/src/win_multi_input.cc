// win_multi_input — see win_multi_input.h. Proposal 35, Phase 3.

#include "win_multi_input.h"

#include "asio_id.h"
#include "wasapi_input.h"

#include "tw/core/twlog.h"

#ifdef TW_HAVE_ASIO
#  include "asio_input.h"
#endif

namespace audio {

WinMultiInput::WinMultiInput()
{
    wasapi_.reset(new WASAPIInput());
    current_ = wasapi_.get();
}

WinMultiInput::~WinMultiInput() = default;

AudioInput *WinMultiInput::active_() const
{
    return current_ ? current_ : wasapi_.get();
}

const char *WinMultiInput::backendName() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return active_() ? active_()->backendName() : "none";
}

int WinMultiInput::openDevice(const std::string &deviceId, std::uint32_t preferredRate)
{
    std::lock_guard<std::mutex> lk(mutex_);
    error_.clear();

    const ParsedDeviceId p = parseDeviceId(deviceId);

    AudioInput *want = nullptr;
    if (p.kind == DeviceIdKind::Asio) {
#ifdef TW_HAVE_ASIO
        if (!asio_) asio_.reset(new AsioInput());
        want = asio_.get();
#else
        error_ = "this build has no ASIO support (the Steinberg SDK was not present at "
                 "configure time)";
        TW_LOGE("devices", "WinMultiInput: device id '%s' selects an ASIO driver, but %s",
                deviceId.c_str(), error_.c_str());
        return -1;
#endif
    } else {
        want = wasapi_.get();
    }

    if (current_ && current_ != want) {
        current_->stopCapture();
        current_->closeDevice();
    }
    current_ = want;

    // A channel request that arrived before anything was open carries across
    // the route change: it is a statement about what the USER wants recorded,
    // not about a particular device.
    if (pendingMask_) current_->requestChannels(pendingMask_);

    const int rc = current_->openDevice(p.native, preferredRate);
    if (rc != 0) {
        const char *why = current_->errorMessage();
        if (why && *why) error_ = why;
        // NO SILENT FALLBACK, for the same reason the output dispatcher has
        // none: a user who chose an ASIO driver and was quietly given the
        // shared-mode one would be recording through the very path they picked
        // ASIO to avoid — and on this platform that path is the one with the
        // endpoint sample-rate trap in it.
        TW_LOGE("devices", "WinMultiInput: open of '%s' failed: %s", deviceId.c_str(),
                error_.c_str());
    }
    return rc;
}

int WinMultiInput::closeDevice()
{
    std::lock_guard<std::mutex> lk(mutex_);
    return active_() ? active_()->closeDevice() : 0;
}

int WinMultiInput::startCapture()
{
    std::lock_guard<std::mutex> lk(mutex_);
    return active_() ? active_()->startCapture() : -1;
}

int WinMultiInput::stopCapture()
{
    std::lock_guard<std::mutex> lk(mutex_);
    return active_() ? active_()->stopCapture() : 0;
}

std::int32_t WinMultiInput::read(float *interleaved, std::size_t frameCount)
{
    // The consumer's hot path: copy the pointer under the lock, then let the
    // backend's own ring do the synchronising.
    AudioInput *in = nullptr;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        in = active_();
    }
    return in ? in->read(interleaved, frameCount) : -1;
}

int WinMultiInput::requestChannels(std::uint64_t mask)
{
    std::lock_guard<std::mutex> lk(mutex_);
    pendingMask_ |= mask;
    return active_() ? active_()->requestChannels(mask) : 0;
}

const AudioInputConfig &WinMultiInput::getConfig() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    // Returns a REFERENCE, so it must be the backend's own object rather than
    // a copy of it in a local.
    static const AudioInputConfig kEmpty{};
    return active_() ? active_()->getConfig() : kEmpty;
}

AudioInputStats WinMultiInput::stats() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return active_() ? active_()->stats() : AudioInputStats{};
}

std::vector<std::uint32_t> WinMultiInput::getAvailableBufferSizes() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return active_() ? active_()->getAvailableBufferSizes() : std::vector<std::uint32_t>{};
}

int WinMultiInput::setBufferSize(std::uint32_t frameCount)
{
    std::lock_guard<std::mutex> lk(mutex_);
    return active_() ? active_()->setBufferSize(frameCount) : -1;
}

const char *WinMultiInput::errorMessage() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (!error_.empty()) return error_.c_str();
    return active_() ? active_()->errorMessage() : "";
}

std::vector<AudioInputDeviceInfo> WinMultiInput::listDevices() const
{
    std::lock_guard<std::mutex> lk(mutex_);

    // WASAPI ids stay BARE — accepted with a `wasapi:` prefix, never emitted
    // with one. The reasoning is win_multi_backend.cc's and applies verbatim:
    // an id that changes spelling is an id a stored selection stops matching.
    std::vector<AudioInputDeviceInfo> out;
    if (wasapi_) {
        for (AudioInputDeviceInfo d : wasapi_->listDevices()) out.push_back(std::move(d));
    }
#ifdef TW_HAVE_ASIO
    {
        AsioInput probe;   // registry only; loads no driver
        for (AudioInputDeviceInfo d : probe.listDevices()) {
            d.name = "ASIO: " + d.name;
            out.push_back(std::move(d));
        }
    }
#endif
    return out;
}

}  // namespace audio
