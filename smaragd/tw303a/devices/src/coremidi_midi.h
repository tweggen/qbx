#ifndef _TW_COREMIDI_MIDI_H_
#define _TW_COREMIDI_MIDI_H_

// PRIVATE to tw_devices. macOS only, built behind QBX_MAC_COREMIDI.

#include "tw/devices/midi_input.h"
#include "tw/devices/midi_output.h"

#include <mutex>

#include <CoreMIDI/CoreMIDI.h>

namespace audio {

// CoreMIDI: the one platform where both things WinMM lacks are native.
//   - VIRTUAL PORTS are first class (MIDISourceCreate / MIDIDestinationCreate),
//     so createVirtualPort() really does publish an endpoint other apps see.
//   - TIMESTAMPS are first class: a MIDIPacket carries a mach host time and the
//     driver does the pacing. supportsTimestamps() is true, so MidiOutScheduler
//     hands messages off as soon as it has them and stops pacing entirely.
//
// UNVERIFIED (proposal 37 P7a was implemented on Windows): this file compiles
// and runs nowhere in the P7a gate. It is written against the documented API
// and reviewed, not executed.
class CoreMidiOutput : public MidiOutput {
public:
    ~CoreMidiOutput() override;

    int  open(const std::string &portId = "default") override;
    int  close() override;
    bool isOpen() const override { return dest_ != 0 || virtualSrc_ != 0; }

    std::vector<MidiPortInfo> listPorts() const override;
    bool createVirtualPort(const std::string &name) override;

    int  send(const std::uint8_t *bytes, std::size_t size,
              std::int64_t hostTimeNs = 0) override;

    bool supportsTimestamps() const override { return true; }
    const char *backendName() const override { return "coremidi"; }

private:
    bool ensureClient();

    MIDIClientRef   client_     = 0;
    MIDIPortRef     port_       = 0;
    MIDIEndpointRef dest_       = 0;   // the endpoint we send TO
    MIDIEndpointRef virtualSrc_ = 0;   // the endpoint we publish, if any
};

class CoreMidiInput : public MidiInput {
public:
    ~CoreMidiInput() override;

    int  open(const std::string &portId = "default") override;
    int  close() override;
    bool isOpen() const override { return port_ != 0 || virtualDest_ != 0; }

    std::vector<MidiPortInfo> listPorts() const override;
    bool createVirtualPort(const std::string &name) override;
    void setCallback(MidiInputCallback cb) override;

    const char *backendName() const override { return "coremidi"; }

private:
    bool ensureClient();
    static void readProc(const MIDIPacketList *pkts, void *readRefCon, void *srcRefCon);
    void deliver(const MIDIPacketList *pkts);

    MIDIClientRef     client_      = 0;
    MIDIPortRef       port_        = 0;
    MIDIEndpointRef   source_      = 0;
    MIDIEndpointRef   virtualDest_ = 0;
    std::mutex        mutex_;
    MidiInputCallback cb_;
};

}  // namespace audio

#endif
