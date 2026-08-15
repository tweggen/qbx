#ifndef _TW_MIDI_OUTPUT_H_
#define _TW_MIDI_OUTPUT_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace audio {

// One selectable MIDI endpoint. `id` is the backend-native handle passed back
// to open() (a WinMM device index as a decimal string, a CoreMIDI endpoint
// uniqueID, an ALSA "client:port" pair); `name` is the label for the UI.
// The empty id / "default" means "the backend's default endpoint".
// `isVirtual` marks a port this process created itself (CoreMIDI/ALSA only).
struct MidiPortInfo {
    std::string id;
    std::string name;
    bool        isVirtual = false;
};

// The output half of the device layer, mirroring AudioBackend's shape: a thin,
// format-agnostic wire for raw MIDI bytes, with the platform behind it.
//
// Threading (proposal 36 D6): every method here is called from EXACTLY ONE
// thread at a time. In the app that thread is MidiOutScheduler's — the
// scheduler is the sole sender, so a backend needs no lock of its own; the
// control-plane calls (open/close/createVirtualPort) happen with the scheduler
// stopped. No implementation may touch Qt: MIDI-out runs off a plain
// std::thread and a Qt signal from it would make Qt adopt the thread
// (THREADING.md rule 1).
class MidiOutput {
public:
    virtual ~MidiOutput() = default;

    // Open a port. "" / "default" = the backend's default endpoint.
    // Returns 0 on success, -1 on error.
    virtual int  open(const std::string &portId = "default") = 0;
    virtual int  close()                                     = 0;
    virtual bool isOpen() const                              = 0;

    // Enumerate the endpoints a device picker may offer. May be called before
    // open(). An empty list means the backend exposes nothing selectable.
    virtual std::vector<MidiPortInfo> listPorts() const       = 0;

    // Create (and open) a virtual source other applications can connect to.
    // Returns false where the platform has no such concept — notably WinMM,
    // where a loopback driver (loopMIDI) is the only route. A UI must gate the
    // offer on this returning false, not assume it.
    virtual bool createVirtualPort(const std::string &name)   = 0;

    // Send one complete MIDI message (channel voice, sysex, realtime).
    // `hostTimeNs` is a MidiOutScheduler::hostNowNs() reading: 0 means "now".
    // A backend that reports supportsTimestamps() == false IGNORES it and
    // sends immediately — the scheduler then withholds the message until it is
    // due. Returns 0 on success, -1 on error.
    virtual int  send(const std::uint8_t *bytes, std::size_t size,
                      std::int64_t hostTimeNs = 0)            = 0;

    // True when the driver can be handed a future-timestamped message and will
    // deliver it itself (CoreMIDI, ALSA sequencer queues). The scheduler hands
    // off early to those and only paces the rest.
    virtual bool supportsTimestamps() const { return false; }

    // Output latency of the port in nanoseconds, if the backend knows it.
    // 0 means "unknown / negligible"; the pump's alignment arithmetic (D6)
    // subtracts it.
    virtual std::int64_t latencyNs() const { return 0; }

    virtual const char *backendName() const = 0;
};

// SMARAGD_MIDI_BACKEND = winmm | coremidi | alsaseq | capture | null | default.
// Same precedence discipline as SMARAGD_AUDIO_BACKEND: the variable outranks
// the platform choice, an unknown value warns and falls back to the platform,
// `default` (or unset) IS the platform. A --test-case run is expected to set it
// to `capture` before the app exists unless it is already set (the app half of
// that lives in main.cpp — proposal 36 P7b).
std::unique_ptr<MidiOutput> createMidiOutput();

// The same selection, named explicitly — for a settings dialog or a test that
// wants one specific backend regardless of the environment.
std::unique_ptr<MidiOutput> createMidiOutput(const std::string &backend);

}  // namespace audio

#endif
