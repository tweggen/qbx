#ifndef _TW_NULL_MIDI_H_
#define _TW_NULL_MIDI_H_

#include "tw/devices/midi_input.h"
#include "tw/devices/midi_output.h"

namespace audio {

// Drop-in MIDI ports for a platform with no MIDI implementation (and the
// fallback when a named backend is not compiled in). Honours the lifecycle and
// NEVER fails: a missing MIDI device must not stop the app from starting, and
// a test that only wants "some MidiOutput" gets one everywhere.
//
// listPorts() is empty because there genuinely is no endpoint; open() still
// succeeds, mirroring NullBackend, whose enumerateDevices() is empty while
// openDevice() returns 0.
class NullMidiOutput : public MidiOutput {
public:
    int  open(const std::string & = "default") override { open_ = true;  return 0; }
    int  close() override                               { open_ = false; return 0; }
    bool isOpen() const override                        { return open_; }

    std::vector<MidiPortInfo> listPorts() const override { return {}; }
    // True: the null port accepts everything, including the request for a
    // virtual one. Nothing downstream can tell the difference, and a UI that
    // gates on this over the null backend is not driving hardware anyway.
    bool createVirtualPort(const std::string &) override { open_ = true; return true; }

    int  send(const std::uint8_t *, std::size_t, std::int64_t = 0) override { return 0; }

    const char *backendName() const override { return "null"; }

private:
    bool open_ = false;
};

class NullMidiInput : public MidiInput {
public:
    int  open(const std::string & = "default") override { open_ = true;  return 0; }
    int  close() override                               { open_ = false; return 0; }
    bool isOpen() const override                        { return open_; }

    std::vector<MidiPortInfo> listPorts() const override { return {}; }
    bool createVirtualPort(const std::string &) override { open_ = true; return true; }
    void setCallback(MidiInputCallback cb) override      { cb_ = std::move(cb); }

    const char *backendName() const override { return "null"; }

private:
    MidiInputCallback cb_;
    bool              open_ = false;
};

}  // namespace audio

#endif
