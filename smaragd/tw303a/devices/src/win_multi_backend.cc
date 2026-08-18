// win_multi_backend — see win_multi_backend.h. Proposal 35, Phase 2.

#include "win_multi_backend.h"

#include "asio_id.h"

#include "tw/devices/wasapi_backend.h"
#include "tw/core/twlog.h"

#ifdef TW_HAVE_ASIO
#  include "asio_backend.h"
#endif

namespace audio {

WinMultiBackend::WinMultiBackend()
{
    // WASAPI exists from the start: it is the default route, and the one a
    // bare persisted id resolves to.
    wasapi_.reset(new WASAPIBackend());
    current_ = wasapi_.get();
}

WinMultiBackend::~WinMultiBackend() = default;

AudioBackend *WinMultiBackend::active_() const
{
    return current_ ? current_ : wasapi_.get();
}

const char *WinMultiBackend::name() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return active_() ? active_()->name() : "none";
}

int WinMultiBackend::openDevice(const std::string &deviceName, std::uint32_t preferredRate)
{
    std::lock_guard<std::mutex> lk(mutex_);

    const ParsedDeviceId p = parseDeviceId(deviceName);

    AudioBackend *want = nullptr;
    if (p.kind == DeviceIdKind::Asio) {
#ifdef TW_HAVE_ASIO
        if (!asio_) asio_.reset(new AsioBackend());
        want = asio_.get();
#else
        // See the header: a build without the SDK still parses the id, so the
        // failure names the cause instead of looking like a broken driver.
        TW_LOGE("devices",
                "WinMultiBackend: device id '%s' selects an ASIO driver, but this build "
                "has no ASIO support (the Steinberg SDK was not present at configure "
                "time). Pick a WASAPI device, or rebuild with the SDK.",
                deviceName.c_str());
        return -1;
#endif
    } else {
        want = wasapi_.get();
    }

    // Switching worlds closes the old one first. An output device is
    // singular — the app plays through one — and leaving a driver open would
    // hold a pro interface against every other application on the machine.
    if (current_ && current_ != want) {
        current_->stopOutput();
        current_->closeDevice();
    }
    current_ = want;

    if (callback_) current_->setRenderCallback(callback_);

    const int rc = current_->openDevice(p.native, preferredRate);
    if (rc != 0) {
#ifdef TW_HAVE_ASIO
        if (p.kind == DeviceIdKind::Asio) {
            const std::string &why = static_cast<AsioBackend *>(current_)->errorMessage();
            if (!why.empty())
                TW_LOGE("devices", "WinMultiBackend: ASIO open failed: %s", why.c_str());
        }
#endif
        // NO SILENT FALLBACK to WASAPI. A user who picked an ASIO driver and
        // silently got the shared-mode one would be listening to the thing
        // they chose ASIO to avoid, with no way to tell. The failure is
        // reported and the caller decides.
        return rc;
    }
    return 0;
}

int WinMultiBackend::closeDevice()
{
    std::lock_guard<std::mutex> lk(mutex_);
    return active_() ? active_()->closeDevice() : 0;
}

int WinMultiBackend::startOutput()
{
    std::lock_guard<std::mutex> lk(mutex_);
    return active_() ? active_()->startOutput() : -1;
}

int WinMultiBackend::stopOutput()
{
    std::lock_guard<std::mutex> lk(mutex_);
    return active_() ? active_()->stopOutput() : 0;
}

bool WinMultiBackend::isRunning() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return active_() && active_()->isRunning();
}

void WinMultiBackend::setRenderCallback(RenderCallback cb)
{
    std::lock_guard<std::mutex> lk(mutex_);
    callback_ = cb;  // kept so a later route change carries it across
    if (active_()) active_()->setRenderCallback(std::move(cb));
}

AudioConfig WinMultiBackend::getConfig() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return active_() ? active_()->getConfig() : AudioConfig{};
}

std::uint32_t WinMultiBackend::getLatencyFrames() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return active_() ? active_()->getLatencyFrames() : 0;
}

std::vector<std::uint32_t> WinMultiBackend::supportedRates() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return active_() ? active_()->supportedRates() : std::vector<std::uint32_t>{};
}

std::vector<std::uint32_t> WinMultiBackend::getAvailableBufferSizes() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return active_() ? active_()->getAvailableBufferSizes() : std::vector<std::uint32_t>{};
}

int WinMultiBackend::setBufferSize(std::uint32_t frameCount)
{
    std::lock_guard<std::mutex> lk(mutex_);
    return active_() ? active_()->setBufferSize(frameCount) : -1;
}

int WinMultiBackend::openControlPanel()
{
    // The active backend answers, and WASAPI's answer is -1 ("no panel") from
    // the base class — which is correct: a shared-mode endpoint's settings live
    // in the Windows sound control panel, not in ours to open.
    AudioBackend *be = nullptr;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        be = active_();
    }
    return be ? be->openControlPanel() : -1;
}

std::vector<AudioDeviceInfo> WinMultiBackend::enumerateDevices() const
{
    std::lock_guard<std::mutex> lk(mutex_);

    // ONE list, WASAPI first: it is the default world, and a user scanning the
    // menu should meet the familiar entries before the driver-specific ones.
    //
    // WASAPI IDS ARE EMITTED BARE, NOT `wasapi:`-PREFIXED. The prefix is
    // ACCEPTED on input (asio_id.h parses it) but never produced, and that
    // asymmetry is deliberate — prefixing here would be a silent regression
    // for every existing user. The device picker checks its menu entries by
    // comparing `AudioDeviceInfo::id` against the CURRENT device string
    // verbatim (`smainwindow.cpp`, `addDevice`), and the Options combo does
    // the same. A user whose smaragd.ini holds a bare endpoint id would match
    // nothing, so the menu would fall back to check-marking the FIRST entry:
    // audio would still come out of the right device (the bare id routes to
    // WASAPI by the fallback rule), but the UI would name the wrong one — and
    // applying the Options page would then actually switch them.
    //
    // Emitting bare keeps enumeration byte-identical to what shipped, so
    // `asio:` is the ONLY new spelling anywhere and there is no migration.
    std::vector<AudioDeviceInfo> out;
    if (wasapi_) {
        for (AudioDeviceInfo d : wasapi_->enumerateDevices())
            out.push_back(std::move(d));
    }
#ifdef TW_HAVE_ASIO
    {
        // Enumerating ASIO does not need an open device, and must not create
        // one: AsioBackend::enumerateDevices reads the registry only.
        AsioBackend probe;
        for (AudioDeviceInfo d : probe.enumerateDevices()) {
            d.name = "ASIO: " + d.name;  // the list is shared; say which world
            out.push_back(std::move(d));
        }
    }
#endif
    return out;
}

}  // namespace audio
