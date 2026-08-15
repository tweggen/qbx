#ifndef _TW_ALSA_SEQ_MIDI_H_
#define _TW_ALSA_SEQ_MIDI_H_

// PRIVATE to tw_devices. Linux only, built behind QBX_LINUX_ALSASEQ.

#include "tw/devices/midi_input.h"
#include "tw/devices/midi_output.h"

#include <atomic>
#include <mutex>
#include <thread>

#include <alsa/asoundlib.h>

namespace audio {

// The ALSA SEQUENCER (snd_seq_*), not the raw MIDI API. The sequencer is what
// gives Linux the two things WinMM lacks:
//   - every application port IS a virtual port (snd_seq_create_simple_port), so
//     createVirtualPort() is the same call open() already makes;
//   - a QUEUE delivers events at a real-time stamp, so supportsTimestamps() is
//     true and MidiOutScheduler hands off early instead of pacing.
//
// UNVERIFIED (proposal 36 P7a was implemented on Windows): written against the
// documented API and reviewed, never executed.
class AlsaSeqMidiOutput : public MidiOutput {
public:
    ~AlsaSeqMidiOutput() override;

    int  open(const std::string &portId = "default") override;
    int  close() override;
    bool isOpen() const override { return seq_ != nullptr; }

    std::vector<MidiPortInfo> listPorts() const override;
    bool createVirtualPort(const std::string &name) override;

    int  send(const std::uint8_t *bytes, std::size_t size,
              std::int64_t hostTimeNs = 0) override;

    bool supportsTimestamps() const override { return true; }
    const char *backendName() const override { return "alsaseq"; }

private:
    bool ensureSeq(const std::string &clientName);
    bool startQueue();

    snd_seq_t         *seq_    = nullptr;
    snd_midi_event_t  *parser_ = nullptr;
    int                port_   = -1;
    int                queue_  = -1;
    std::int64_t       queueEpochNs_ = 0;   // hostNowNs() when the queue started
    bool               connected_    = false;
    int                destClient_   = -1;
    int                destPort_     = -1;
};

class AlsaSeqMidiInput : public MidiInput {
public:
    ~AlsaSeqMidiInput() override;

    int  open(const std::string &portId = "default") override;
    int  close() override;
    bool isOpen() const override { return seq_ != nullptr; }

    std::vector<MidiPortInfo> listPorts() const override;
    bool createVirtualPort(const std::string &name) override;
    void setCallback(MidiInputCallback cb) override;

    const char *backendName() const override { return "alsaseq"; }

private:
    bool ensureSeq(const std::string &clientName);
    void pollLoop();

    snd_seq_t        *seq_    = nullptr;
    snd_midi_event_t *parser_ = nullptr;
    int               port_   = -1;
    std::thread       thread_;
    std::atomic<bool> running_{ false };
    std::mutex        mutex_;
    MidiInputCallback cb_;
};

}  // namespace audio

#endif
