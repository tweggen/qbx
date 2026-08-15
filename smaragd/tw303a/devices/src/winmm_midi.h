#ifndef _TW_WINMM_MIDI_H_
#define _TW_WINMM_MIDI_H_

// PRIVATE to tw_devices (src/, never include/): the Windows MIDI backend, the
// same discipline the WASAPI backend's input half follows.

#include "tw/devices/midi_input.h"
#include "tw/devices/midi_output.h"

#include <mutex>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>

namespace audio {

// WinMM: the oldest and most universally present Windows MIDI API.
//
// Two things it does NOT have, and this class does not pretend otherwise:
//   - VIRTUAL PORTS. Windows has no OS-level concept of an application-created
//     MIDI endpoint; a loopback driver (loopMIDI, LoopBe) supplies them and
//     they then appear as ordinary devices. createVirtualPort() returns false.
//   - TIMESTAMPED SEND. midiOutShortMsg sends NOW. supportsTimestamps() is
//     false, so MidiOutScheduler holds each message until it is due and eats
//     the ±1 ms of pacing jitter itself (proposal 36 D6, explicitly not gated).
class WinMMMidiOutput : public MidiOutput {
public:
    ~WinMMMidiOutput() override;

    int  open(const std::string &portId = "default") override;
    int  close() override;
    bool isOpen() const override { return handle_ != nullptr; }

    std::vector<MidiPortInfo> listPorts() const override;
    bool createVirtualPort(const std::string &name) override;

    int  send(const std::uint8_t *bytes, std::size_t size,
              std::int64_t hostTimeNs = 0) override;

    const char *backendName() const override { return "winmm"; }

private:
    HMIDIOUT handle_ = nullptr;
};

class WinMMMidiInput : public MidiInput {
public:
    ~WinMMMidiInput() override;

    int  open(const std::string &portId = "default") override;
    int  close() override;
    bool isOpen() const override { return handle_ != nullptr; }

    std::vector<MidiPortInfo> listPorts() const override;
    bool createVirtualPort(const std::string &name) override;
    void setCallback(MidiInputCallback cb) override;

    const char *backendName() const override { return "winmm"; }

private:
    static void CALLBACK midiInProc(HMIDIIN h, UINT msg, DWORD_PTR instance,
                                    DWORD_PTR p1, DWORD_PTR p2);
    void deliver(DWORD packed);

    HMIDIIN            handle_ = nullptr;
    std::mutex         mutex_;          // guards cb_ against the device thread
    MidiInputCallback  cb_;
};

}  // namespace audio

#endif
