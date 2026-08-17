#ifndef _TW_KEYBOARD_MIDI_H_
#define _TW_KEYBOARD_MIDI_H_

#include "tw/devices/midi_input.h"

#include <atomic>
#include <cstdint>
#include <mutex>

namespace audio {

// THE COMPUTER KEYBOARD AS A REAL MIDI INPUT PORT (proposal 21 L2, design D9).
//
// "SVirtualKeyboardDock becomes an in-process MidiInput port so live play and
// recording treat it like hardware." That is the whole idea, and it is worth
// being explicit about why it is a DEVICE and not a shortcut into the live
// lane: every consumer downstream - the fan-out, the per-consumer rings, the
// live event source's host-time mapping, the MIDI recorder in L4, MIDI-thru -
// then has exactly ONE shape to handle. A second, privileged path for the
// keyboard would be a second set of bugs.
//
// It is NOT selected by SMARAGD_MIDI_BACKEND: the backend variable chooses the
// SYSTEM MIDI implementation, and the computer keyboard exists on every
// machine whatever that choice is. `createMidiInput("keyboard")` names it
// explicitly, and it enumerates itself as one port so the Options page and a
// track's input selector can offer it beside the hardware ones.
//
// Threading: noteOn/noteOff/inject may be called from any thread (in practice
// the main thread, from the dock's key handler). The callback runs on the
// CALLING thread, exactly as CaptureMidiInput::inject does - which is what
// makes it the "device thread" as far as MidiInFanout is concerned. Nothing
// here touches Qt.
class KeyboardMidiInput : public MidiInput {
public:
    // The port id and the portable NAME. `kPortId` is what a track's
    // `trackInput=keyboard` resolves to and what listPorts() reports.
    static constexpr const char *kPortId   = "keyboard";
    static constexpr const char *kPortName = "Computer keyboard";

    KeyboardMidiInput();
    ~KeyboardMidiInput() override;

    int  open(const std::string &portId = "keyboard") override;
    int  close() override;
    bool isOpen() const override { return open_.load(std::memory_order_acquire); }

    std::vector<MidiPortInfo> listPorts() const override;
    bool createVirtualPort(const std::string &name) override;
    void setCallback(MidiInputCallback cb) override;

    const char *backendName() const override { return "keyboard"; }

    // The dock's hand. `velocity` is the MIDI 0..127 domain, matching
    // `add-note velocity=` and `midi-in-event velocity=`.
    void noteOn(int key, int velocity, int channel = 0);
    void noteOff(int key, int channel = 0);
    // Everything the two above cannot spell (a CC from a mapped key, later).
    void inject(const std::uint8_t *bytes, std::size_t size,
                std::int64_t hostTimeNs = 0);

    // The live instance, for the app and the testkit - the same accessor shape
    // CaptureMidiInput::active() uses, and for the same reason: the port is
    // owned several layers away from the widget that plays it.
    static KeyboardMidiInput *active();

private:
    mutable std::mutex mutex_;
    MidiInputCallback  cb_;
    std::atomic<bool>  open_{ false };
};

}  // namespace audio

#endif
