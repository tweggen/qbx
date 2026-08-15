#include "winmm_midi.h"

#include "tw/devices/midi_out_scheduler.h"   // hostNowNs
#include "tw/core/twsyslog.h"

#include <cstdlib>
#include <string>
#include <vector>

namespace audio {

namespace {

// Length of a complete MIDI message from its status byte. Used by the INPUT
// path, where WinMM hands us three packed bytes and expects us to know how many
// of them are real.
std::size_t messageLength(std::uint8_t status)
{
    if (status < 0x80) return 0;                      // not a status byte
    switch (status & 0xF0) {
    case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0: return 3;
    case 0xC0: case 0xD0:                                  return 2;
    default: break;
    }
    switch (status) {
    case 0xF1: case 0xF3: return 2;                   // MTC quarter frame, song select
    case 0xF2:            return 3;                   // song position
    default:              return 1;                   // realtime / tune request
    }
}

// "default"/"" means the MIDI Mapper (the user's Windows-wide default synth);
// anything else is the decimal device index listPorts() reported.
bool resolveDeviceId(const std::string &portId, UINT deviceCount, UINT &out)
{
    if (portId.empty() || portId == "default") {
        out = (UINT) MIDI_MAPPER;
        return true;
    }
    char *end = nullptr;
    const long v = std::strtol(portId.c_str(), &end, 10);
    if (end == portId.c_str() || *end != '\0' || v < 0 || (UINT) v >= deviceCount)
        return false;
    out = (UINT) v;
    return true;
}

}  // namespace

// --- output -----------------------------------------------------------------

WinMMMidiOutput::~WinMMMidiOutput()
{
    close();
}

int WinMMMidiOutput::open(const std::string &portId)
{
    close();

    UINT id = 0;
    if (!resolveDeviceId(portId, midiOutGetNumDevs(), id)) {
        syslog(LOG_WARNING, "midi: WinMM has no output port '%s'", portId.c_str());
        return -1;
    }

    HMIDIOUT h  = nullptr;
    const MMRESULT r = midiOutOpen(&h, id, 0, 0, CALLBACK_NULL);
    if (r != MMSYSERR_NOERROR) {
        syslog(LOG_WARNING, "midi: midiOutOpen('%s') failed with %u", portId.c_str(),
               (unsigned) r);
        return -1;
    }
    handle_ = h;
    return 0;
}

int WinMMMidiOutput::close()
{
    if (!handle_) return 0;
    // Reset first: it silences anything the port is still holding, so a crash
    // or a hard close cannot leave a note ringing on the hardware.
    midiOutReset(handle_);
    midiOutClose(handle_);
    handle_ = nullptr;
    return 0;
}

std::vector<MidiPortInfo> WinMMMidiOutput::listPorts() const
{
    std::vector<MidiPortInfo> ports;
    const UINT n = midiOutGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
        MIDIOUTCAPSA caps{};
        if (midiOutGetDevCapsA(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
        ports.push_back(MidiPortInfo{ std::to_string(i), std::string(caps.szPname), false });
    }
    return ports;
}

bool WinMMMidiOutput::createVirtualPort(const std::string & /*name*/)
{
    // Not a failure to report loudly — it is a platform fact (see the header).
    return false;
}

int WinMMMidiOutput::send(const std::uint8_t *bytes, std::size_t size,
                          std::int64_t /*hostTimeNs*/)
{
    if (!handle_ || !bytes || size == 0) return -1;

    if (bytes[0] == 0xF0) {
        // Sysex. midiOutLongMsg is asynchronous, so the header must stay alive
        // until the driver is done with it; unprepare answers MIDIERR_STILLPLAYING
        // until then. The sender thread is the only caller and sysex is rare, so
        // waiting here is simpler and safer than a completion queue.
        std::vector<std::uint8_t> buf(bytes, bytes + size);
        MIDIHDR hdr{};
        hdr.lpData         = (LPSTR) buf.data();
        hdr.dwBufferLength = (DWORD) buf.size();
        if (midiOutPrepareHeader(handle_, &hdr, sizeof(hdr)) != MMSYSERR_NOERROR) return -1;
        const MMRESULT r = midiOutLongMsg(handle_, &hdr, sizeof(hdr));
        for (int i = 0; i < 1000; ++i) {
            if (midiOutUnprepareHeader(handle_, &hdr, sizeof(hdr)) != MIDIERR_STILLPLAYING)
                break;
            Sleep(1);
        }
        return r == MMSYSERR_NOERROR ? 0 : -1;
    }

    // Short message: status in the low byte, then the data bytes.
    DWORD packed = 0;
    for (std::size_t i = 0; i < size && i < 3; ++i)
        packed |= (DWORD) bytes[i] << (8 * i);
    return midiOutShortMsg(handle_, packed) == MMSYSERR_NOERROR ? 0 : -1;
}

// --- input ------------------------------------------------------------------

WinMMMidiInput::~WinMMMidiInput()
{
    close();
}

int WinMMMidiInput::open(const std::string &portId)
{
    close();

    const UINT n = midiInGetNumDevs();
    UINT id = 0;
    if (portId.empty() || portId == "default") {
        if (n == 0) {
            syslog(LOG_INFO, "midi: no WinMM input devices present");
            return -1;
        }
        id = 0;                       // there is no MIDI_MAPPER for input
    } else if (!resolveDeviceId(portId, n, id) || id == (UINT) MIDI_MAPPER) {
        syslog(LOG_WARNING, "midi: WinMM has no input port '%s'", portId.c_str());
        return -1;
    }

    HMIDIIN h = nullptr;
    const MMRESULT r = midiInOpen(&h, id, (DWORD_PTR) &WinMMMidiInput::midiInProc,
                                  (DWORD_PTR) this, CALLBACK_FUNCTION);
    if (r != MMSYSERR_NOERROR) {
        syslog(LOG_WARNING, "midi: midiInOpen('%s') failed with %u", portId.c_str(),
               (unsigned) r);
        return -1;
    }
    handle_ = h;
    midiInStart(handle_);
    return 0;
}

int WinMMMidiInput::close()
{
    if (!handle_) return 0;
    midiInStop(handle_);
    midiInReset(handle_);
    midiInClose(handle_);
    handle_ = nullptr;
    return 0;
}

std::vector<MidiPortInfo> WinMMMidiInput::listPorts() const
{
    std::vector<MidiPortInfo> ports;
    const UINT n = midiInGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
        MIDIINCAPSA caps{};
        if (midiInGetDevCapsA(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
        ports.push_back(MidiPortInfo{ std::to_string(i), std::string(caps.szPname), false });
    }
    return ports;
}

bool WinMMMidiInput::createVirtualPort(const std::string & /*name*/)
{
    return false;
}

void WinMMMidiInput::setCallback(MidiInputCallback cb)
{
    std::lock_guard<std::mutex> lock(mutex_);
    cb_ = std::move(cb);
}

void CALLBACK WinMMMidiInput::midiInProc(HMIDIIN /*h*/, UINT msg, DWORD_PTR instance,
                                         DWORD_PTR p1, DWORD_PTR /*p2*/)
{
    // MIM_DATA only. Sysex arrives as MIM_LONGDATA against buffers we would have
    // to add with midiInAddBuffer; proposal 36 P8 adds that when there is a
    // consumer for it. Everything else (MIM_OPEN/CLOSE/ERROR) is ignored.
    if (msg != MIM_DATA) return;
    auto *self = reinterpret_cast<WinMMMidiInput *>(instance);
    if (self) self->deliver((DWORD) p1);
}

void WinMMMidiInput::deliver(DWORD packed)
{
    const std::uint8_t bytes[3] = { (std::uint8_t)(packed & 0xFF),
                                    (std::uint8_t)((packed >> 8) & 0xFF),
                                    (std::uint8_t)((packed >> 16) & 0xFF) };
    const std::size_t len = messageLength(bytes[0]);
    if (len == 0) return;

    // WinMM's own timestamp is milliseconds since midiInStart, in a different
    // epoch from everything else in this app. hostNowNs() is THE clock (see
    // MidiOutScheduler), and the callback runs on the device thread within a
    // millisecond of arrival, so reading it here is both consistent and honest.
    const std::int64_t now = MidiOutScheduler::hostNowNs();

    MidiInputCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = cb_;
    }
    // Called with NO lock held: this is the device thread, and it must not be
    // able to block on anything the main thread holds (THREADING.md rule 3).
    if (cb) cb(bytes, len, now);
}

}  // namespace audio
