#ifndef _TW_MIDI_INPUT_H_
#define _TW_MIDI_INPUT_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "tw/devices/midi_output.h"   // MidiPortInfo

namespace audio {

// Delivered ON THE DEVICE THREAD (the WinMM callback, the CoreMIDI read proc,
// the ALSA sequencer poll thread). It must be Qt-free, allocation-light and
// non-blocking: proposal 37 P8 pushes into an SPSC ring from here and does the
// model work on the main thread.
using MidiInputCallback =
    std::function<void(const std::uint8_t *bytes, std::size_t size,
                       std::int64_t hostTimeNs)>;

// The input half of the device layer. Mirrors MidiOutput; the same threading
// rules apply, with the addition that the callback runs on a thread this
// process does not own.
class MidiInput {
public:
    virtual ~MidiInput() = default;

    virtual int  open(const std::string &portId = "default") = 0;
    virtual int  close()                                     = 0;
    virtual bool isOpen() const                              = 0;

    virtual std::vector<MidiPortInfo> listPorts() const       = 0;

    // Create (and open) a virtual destination other applications can send to.
    // False where the platform has no such concept (WinMM).
    virtual bool createVirtualPort(const std::string &name)   = 0;

    // Set before open(); setting it while open is allowed but races the device
    // thread by definition, so callers do it with the port closed.
    virtual void setCallback(MidiInputCallback cb)            = 0;

    virtual const char *backendName() const = 0;
};

// Selected by SMARAGD_MIDI_BACKEND exactly like createMidiOutput().
std::unique_ptr<MidiInput> createMidiInput();
std::unique_ptr<MidiInput> createMidiInput(const std::string &backend);

}  // namespace audio

#endif
