#ifndef _TW_CAPTURE_MIDI_H_
#define _TW_CAPTURE_MIDI_H_

#include "tw/devices/midi_input.h"
#include "tw/devices/midi_output.h"

#include <atomic>
#include <mutex>

namespace audio {

// A MIDI port that records into MEMORY instead of reaching a synth.
//
// Why it exists (proposal 37 D6, review #12): the MIDI-out pump has to be
// measurable without a MIDI device and, more importantly, WITHOUT measuring
// itself. So this backend records only what it can observe — the wall-clock
// instant a message actually arrived at the wire, and the bytes — and the
// testkit converts that host time into a project frame through the AUDIO
// capture backend's block log (CaptureBackend::frameAtHostTime). The two logs
// are produced by different threads from the same steady clock, so the
// assertion is "the pump agreed with the audio clock", not "the pump agreed
// with the pump".
//
// It therefore reports supportsTimestamps() == false ON PURPOSE. Reporting
// true would let MidiOutScheduler hand messages off early with a due time
// attached, and the recorded instant would become the handoff instant — which
// would make every latency assertion trivially pass and measure nothing.
class CaptureMidiOutput : public MidiOutput {
public:
    struct Event {
        std::int64_t              hostTimeNs = 0;   // when send() was entered
        std::string               port;             // the port id open() got
        std::vector<std::uint8_t> bytes;
    };

    CaptureMidiOutput();
    ~CaptureMidiOutput() override;

    int  open(const std::string &portId = "default") override;
    int  close() override;
    bool isOpen() const override { return open_; }

    std::vector<MidiPortInfo> listPorts() const override;
    bool createVirtualPort(const std::string &name) override;

    int  send(const std::uint8_t *bytes, std::size_t size,
              std::int64_t hostTimeNs = 0) override;

    bool supportsTimestamps() const override { return false; }
    const char *backendName() const override { return "capture"; }

    // A COPY under the recorder's lock: the scheduler thread appends while a
    // caller reads, and a vector that reallocates under a reader is a dangling
    // span (same rule as CaptureBackend::capturedAudio).
    std::vector<Event> captured() const;
    std::size_t        capturedCount() const;
    void               clear();

    // The live instance, for the app/testkit — the MIDI analogue of
    // dynamic_cast<CaptureBackend*>(speaker's backend), which is how the audio
    // capture recording is reached. A static accessor rather than a cast
    // because the MidiOutput is owned by MidiOutScheduler, several layers away
    // from anything a testkit action holds. The most recently CONSTRUCTED live
    // instance wins; a destroyed instance clears the pointer only if it is
    // still the registered one.
    static CaptureMidiOutput *active();

private:
    mutable std::mutex  mutex_;
    std::vector<Event>  events_;
    std::string         portId_ = "capture";
    bool                open_   = false;
};

// The input counterpart: a port that never delivers anything by itself, but
// whose inject() lets a headless case feed bytes to the callback as if a device
// had. Proposal 37 P8 uses it as the headless input source; P7a ships it so the
// interface has a working implementation on every platform.
class CaptureMidiInput : public MidiInput {
public:
    CaptureMidiInput();
    ~CaptureMidiInput() override;

    int  open(const std::string &portId = "default") override;
    int  close() override;
    bool isOpen() const override { return open_; }

    std::vector<MidiPortInfo> listPorts() const override;
    bool createVirtualPort(const std::string &name) override;
    void setCallback(MidiInputCallback cb) override;

    const char *backendName() const override { return "capture"; }

    // Deliver bytes to the callback as a device would, stamped with the
    // caller's clock (0 = now). Callable from any thread.
    void inject(const std::uint8_t *bytes, std::size_t size,
                std::int64_t hostTimeNs = 0);

    static CaptureMidiInput *active();

private:
    mutable std::mutex mutex_;
    MidiInputCallback  cb_;
    bool               open_ = false;
};

}  // namespace audio

#endif
