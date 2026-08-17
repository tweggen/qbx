#include "tw/devices/capture_midi.h"

#include "tw/devices/midi_out_scheduler.h"   // hostNowNs — ONE clock for MIDI

#include "tw/core/twsyslog.h"

#include <algorithm>

namespace audio {

namespace {

// The live instances, for active(). Plain atomics rather than a registry: the
// app builds exactly one of each, and a test that builds several gets the most
// recent one — stated in the header rather than defended with machinery.
std::atomic<CaptureMidiOutput *> g_activeOut{ nullptr };
std::atomic<CaptureMidiInput *>  g_activeIn{ nullptr };

}  // namespace

// --- output -----------------------------------------------------------------

CaptureMidiOutput::CaptureMidiOutput()
{
    g_activeOut.store(this, std::memory_order_release);
}

CaptureMidiOutput::~CaptureMidiOutput()
{
    CaptureMidiOutput *self = this;
    g_activeOut.compare_exchange_strong(self, nullptr, std::memory_order_acq_rel);
}

CaptureMidiOutput *CaptureMidiOutput::active()
{
    return g_activeOut.load(std::memory_order_acquire);
}

int CaptureMidiOutput::open(const std::string &portId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    portId_ = portId.empty() ? std::string("capture") : portId;
    open_   = true;
    syslog(LOG_INFO,
           "midi: CaptureMidiOutput active on port '%s' — messages are recorded to memory,"
           " not sent to a device.",
           portId_.c_str());
    return 0;
}

int CaptureMidiOutput::close()
{
    std::lock_guard<std::mutex> lock(mutex_);
    open_ = false;
    // The recording deliberately SURVIVES the close, exactly like the audio
    // CaptureBackend's: a case stops the transport and only then dumps.
    return 0;
}

std::vector<MidiPortInfo> CaptureMidiOutput::listPorts() const
{
    // One synthetic endpoint, never an empty list: an Options page showing the
    // machine's real MIDI ports while this backend is active would be a lie.
    return { MidiPortInfo{ "capture", "Capture (in-memory test port)", false } };
}

bool CaptureMidiOutput::createVirtualPort(const std::string &name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    portId_ = name.empty() ? std::string("capture") : name;
    open_   = true;
    return true;
}

int CaptureMidiOutput::send(const std::uint8_t *bytes, std::size_t size,
                            std::int64_t /*hostTimeNs*/)
{
    if (!bytes || size == 0) return -1;

    // The instant the message reached the wire, NOT the due time it was asked
    // for. That difference is the measurement (see the header).
    Event ev;
    ev.hostTimeNs = MidiOutScheduler::hostNowNs();
    ev.bytes.assign(bytes, bytes + size);

    std::lock_guard<std::mutex> lock(mutex_);
    ev.port = portId_;
    events_.push_back(std::move(ev));
    return 0;
}

std::vector<CaptureMidiOutput::Event> CaptureMidiOutput::captured() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return events_;
}

std::size_t CaptureMidiOutput::capturedCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return events_.size();
}

void CaptureMidiOutput::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    events_.clear();
}

// --- input ------------------------------------------------------------------

CaptureMidiInput::CaptureMidiInput()
{
    g_activeIn.store(this, std::memory_order_release);
}

CaptureMidiInput::~CaptureMidiInput()
{
    CaptureMidiInput *self = this;
    g_activeIn.compare_exchange_strong(self, nullptr, std::memory_order_acq_rel);
}

CaptureMidiInput *CaptureMidiInput::active()
{
    return g_activeIn.load(std::memory_order_acquire);
}

int CaptureMidiInput::open(const std::string & /*portId*/)
{
    std::lock_guard<std::mutex> lock(mutex_);
    open_ = true;
    return 0;
}

int CaptureMidiInput::close()
{
    std::lock_guard<std::mutex> lock(mutex_);
    open_ = false;
    return 0;
}

std::vector<MidiPortInfo> CaptureMidiInput::listPorts() const
{
    return { MidiPortInfo{ "capture", "Capture (in-memory test port)", false } };
}

bool CaptureMidiInput::createVirtualPort(const std::string & /*name*/)
{
    std::lock_guard<std::mutex> lock(mutex_);
    open_ = true;
    return true;
}

void CaptureMidiInput::setCallback(MidiInputCallback cb)
{
    std::lock_guard<std::mutex> lock(mutex_);
    cb_ = std::move(cb);
}

void CaptureMidiInput::inject(const std::uint8_t *bytes, std::size_t size,
                              std::int64_t hostTimeNs)
{
    if (!bytes || size == 0) return;

    MidiInputCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_) return;
        cb = cb_;               // copy, then call with NO lock held: a callback
    }                           // that re-enters would otherwise self-deadlock.
    if (cb) cb(bytes, size, hostTimeNs ? hostTimeNs : MidiOutScheduler::hostNowNs());
}

}  // namespace audio
