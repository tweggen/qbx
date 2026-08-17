#include "alsa_seq_midi.h"

#include "tw/devices/midi_out_scheduler.h"   // hostNowNs
#include "tw/core/twsyslog.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <poll.h>

namespace audio {

namespace {

// "client:port" (aconnect's spelling), or "default" for the first writable
// destination the sequencer offers.
bool parseAddr(const std::string &s, int &client, int &port)
{
    const std::size_t colon = s.find(':');
    if (colon == std::string::npos) return false;
    char *e1 = nullptr;
    char *e2 = nullptr;
    const long c = std::strtol(s.substr(0, colon).c_str(), &e1, 10);
    const long p = std::strtol(s.substr(colon + 1).c_str(), &e2, 10);
    if (c < 0 || p < 0) return false;
    client = (int) c;
    port   = (int) p;
    return true;
}

// Ports a MIDI-out can send TO: they must accept writes and subscriptions.
constexpr unsigned kDestCaps = SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE;
// Ports a MIDI-in can read FROM.
constexpr unsigned kSrcCaps  = SND_SEQ_PORT_CAP_READ  | SND_SEQ_PORT_CAP_SUBS_READ;

std::vector<MidiPortInfo> enumerate(snd_seq_t *seq, unsigned caps, int selfClient)
{
    std::vector<MidiPortInfo> ports;
    if (!seq) return ports;

    snd_seq_client_info_t *cinfo = nullptr;
    snd_seq_port_info_t   *pinfo = nullptr;
    snd_seq_client_info_alloca(&cinfo);
    snd_seq_port_info_alloca(&pinfo);

    snd_seq_client_info_set_client(cinfo, -1);
    while (snd_seq_query_next_client(seq, cinfo) >= 0) {
        const int client = snd_seq_client_info_get_client(cinfo);
        if (client == selfClient) continue;              // never offer our own port

        snd_seq_port_info_set_client(pinfo, client);
        snd_seq_port_info_set_port(pinfo, -1);
        while (snd_seq_query_next_port(seq, pinfo) >= 0) {
            const unsigned have = (unsigned) snd_seq_port_info_get_capability(pinfo);
            if ((have & caps) != caps) continue;

            char id[32];
            std::snprintf(id, sizeof(id), "%d:%d", client,
                          snd_seq_port_info_get_port(pinfo));
            std::string name = snd_seq_client_info_get_name(cinfo);
            name += " — ";
            name += snd_seq_port_info_get_name(pinfo);
            ports.push_back(MidiPortInfo{ id, name, false });
        }
    }
    return ports;
}

}  // namespace

// --- output -----------------------------------------------------------------

AlsaSeqMidiOutput::~AlsaSeqMidiOutput()
{
    close();
}

bool AlsaSeqMidiOutput::ensureSeq(const std::string &clientName)
{
    if (seq_) return true;
    if (snd_seq_open(&seq_, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0) {
        syslog(LOG_WARNING, "midi: snd_seq_open failed; ALSA sequencer unavailable");
        seq_ = nullptr;
        return false;
    }
    snd_seq_set_client_name(seq_, clientName.c_str());

    // OUR port is by definition a virtual one: anything can subscribe to it.
    port_ = snd_seq_create_simple_port(seq_, "Smaragd Out",
                                       SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                                       SND_SEQ_PORT_TYPE_MIDI_GENERIC |
                                           SND_SEQ_PORT_TYPE_APPLICATION);
    if (port_ < 0) {
        syslog(LOG_WARNING, "midi: snd_seq_create_simple_port failed");
        snd_seq_close(seq_);
        seq_ = nullptr;
        return false;
    }

    if (snd_midi_event_new(256, &parser_) < 0) {
        parser_ = nullptr;
    } else {
        // No running status on the wire and no "no-op" events: every message we
        // hand ALSA is complete on its own.
        snd_midi_event_no_status(parser_, 1);
    }
    return startQueue();
}

bool AlsaSeqMidiOutput::startQueue()
{
    queue_ = snd_seq_alloc_named_queue(seq_, "smaragd-midi-out");
    if (queue_ < 0) {
        queue_ = -1;
        syslog(LOG_WARNING, "midi: snd_seq_alloc_named_queue failed; sending unscheduled");
        return true;     // still usable, just without driver-side timing
    }
    snd_seq_start_queue(seq_, queue_, nullptr);
    snd_seq_drain_output(seq_);
    // The queue's real-time clock starts at zero HERE; every due time is
    // expressed as a delta from this instant.
    queueEpochNs_ = MidiOutScheduler::hostNowNs();
    return true;
}

int AlsaSeqMidiOutput::open(const std::string &portId)
{
    if (!ensureSeq("Smaragd")) return -1;

    int client = -1;
    int port   = -1;
    if (portId.empty() || portId == "default") {
        const auto ports = enumerate(seq_, kDestCaps, snd_seq_client_id(seq_));
        if (ports.empty()) {
            // No hardware/software destination — but our own port exists and
            // something may subscribe to it later, so this is not a failure.
            syslog(LOG_INFO, "midi: no ALSA sequencer destination; publishing our port only");
            return 0;
        }
        if (!parseAddr(ports.front().id, client, port)) return -1;
    } else if (!parseAddr(portId, client, port)) {
        syslog(LOG_WARNING, "midi: '%s' is not an ALSA client:port address", portId.c_str());
        return -1;
    }

    if (snd_seq_connect_to(seq_, port_, client, port) < 0) {
        syslog(LOG_WARNING, "midi: snd_seq_connect_to(%d:%d) failed", client, port);
        return -1;
    }
    connected_  = true;
    destClient_ = client;
    destPort_   = port;
    return 0;
}

int AlsaSeqMidiOutput::close()
{
    if (!seq_) return 0;
    if (connected_ && destClient_ >= 0)
        snd_seq_disconnect_to(seq_, port_, destClient_, destPort_);
    if (queue_ >= 0) {
        snd_seq_stop_queue(seq_, queue_, nullptr);
        snd_seq_drain_output(seq_);
        snd_seq_free_queue(seq_, queue_);
        queue_ = -1;
    }
    if (parser_) { snd_midi_event_free(parser_); parser_ = nullptr; }
    snd_seq_close(seq_);
    seq_        = nullptr;
    port_       = -1;
    connected_  = false;
    destClient_ = destPort_ = -1;
    return 0;
}

std::vector<MidiPortInfo> AlsaSeqMidiOutput::listPorts() const
{
    if (!seq_) {
        // Enumeration must work before open(), so borrow a throwaway handle.
        snd_seq_t *tmp = nullptr;
        if (snd_seq_open(&tmp, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0) return {};
        auto ports = enumerate(tmp, kDestCaps, snd_seq_client_id(tmp));
        snd_seq_close(tmp);
        return ports;
    }
    return enumerate(seq_, kDestCaps, snd_seq_client_id(seq_));
}

bool AlsaSeqMidiOutput::createVirtualPort(const std::string &name)
{
    // Creating the sequencer port IS creating a virtual port on ALSA; other
    // applications subscribe to it with aconnect. Nothing else to do.
    return ensureSeq(name.empty() ? std::string("Smaragd") : name);
}

int AlsaSeqMidiOutput::send(const std::uint8_t *bytes, std::size_t size,
                            std::int64_t hostTimeNs)
{
    if (!seq_ || !parser_ || !bytes || size == 0) return -1;

    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_midi_event_reset_encode(parser_);
    if (snd_midi_event_encode(parser_, bytes, (long) size, &ev) <= 0) return -1;
    if (ev.type == SND_SEQ_EVENT_NONE) return -1;      // incomplete message

    snd_seq_ev_set_source(&ev, port_);
    snd_seq_ev_set_subs(&ev);

    if (queue_ >= 0 && hostTimeNs > 0) {
        std::int64_t relNs = hostTimeNs - queueEpochNs_;
        if (relNs < 0) relNs = 0;
        snd_seq_real_time_t t;
        t.tv_sec  = (unsigned int) (relNs / 1000000000LL);
        t.tv_nsec = (unsigned int) (relNs % 1000000000LL);
        // Absolute against the queue's own clock — the driver, not us, waits.
        snd_seq_ev_schedule_real(&ev, queue_, 0, &t);
    } else {
        snd_seq_ev_set_direct(&ev);
    }

    if (snd_seq_event_output(seq_, &ev) < 0) return -1;
    return snd_seq_drain_output(seq_) < 0 ? -1 : 0;
}

// --- input ------------------------------------------------------------------

AlsaSeqMidiInput::~AlsaSeqMidiInput()
{
    close();
}

bool AlsaSeqMidiInput::ensureSeq(const std::string &clientName)
{
    if (seq_) return true;
    if (snd_seq_open(&seq_, "default", SND_SEQ_OPEN_INPUT, 0) < 0) {
        seq_ = nullptr;
        return false;
    }
    snd_seq_set_client_name(seq_, clientName.c_str());
    port_ = snd_seq_create_simple_port(seq_, "Smaragd In",
                                       SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                                       SND_SEQ_PORT_TYPE_MIDI_GENERIC |
                                           SND_SEQ_PORT_TYPE_APPLICATION);
    if (port_ < 0) {
        snd_seq_close(seq_);
        seq_ = nullptr;
        return false;
    }
    if (snd_midi_event_new(256, &parser_) < 0) parser_ = nullptr;
    return true;
}

int AlsaSeqMidiInput::open(const std::string &portId)
{
    if (!ensureSeq("Smaragd")) return -1;

    if (!(portId.empty() || portId == "default")) {
        int client = -1, port = -1;
        if (!parseAddr(portId, client, port)) return -1;
        if (snd_seq_connect_from(seq_, port_, client, port) < 0) {
            syslog(LOG_WARNING, "midi: snd_seq_connect_from(%d:%d) failed", client, port);
            return -1;
        }
    }

    if (!running_.load(std::memory_order_acquire)) {
        running_.store(true, std::memory_order_release);
        thread_ = std::thread([this] { pollLoop(); });
    }
    return 0;
}

int AlsaSeqMidiInput::close()
{
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) thread_.join();     // Qt-free join, as everywhere here
    if (parser_) { snd_midi_event_free(parser_); parser_ = nullptr; }
    if (seq_)    { snd_seq_close(seq_); seq_ = nullptr; }
    port_ = -1;
    return 0;
}

std::vector<MidiPortInfo> AlsaSeqMidiInput::listPorts() const
{
    if (!seq_) {
        snd_seq_t *tmp = nullptr;
        if (snd_seq_open(&tmp, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0) return {};
        auto ports = enumerate(tmp, kSrcCaps, snd_seq_client_id(tmp));
        snd_seq_close(tmp);
        return ports;
    }
    return enumerate(seq_, kSrcCaps, snd_seq_client_id(seq_));
}

bool AlsaSeqMidiInput::createVirtualPort(const std::string &name)
{
    return ensureSeq(name.empty() ? std::string("Smaragd") : name);
}

void AlsaSeqMidiInput::setCallback(MidiInputCallback cb)
{
    std::lock_guard<std::mutex> lock(mutex_);
    cb_ = std::move(cb);
}

void AlsaSeqMidiInput::pollLoop()
{
    // A poll() with a bounded timeout rather than a blocking read: close() must
    // be able to join this thread promptly, and a blocked snd_seq_event_input
    // would only return when an event happens to arrive.
    while (running_.load(std::memory_order_acquire)) {
        const int npfds = snd_seq_poll_descriptors_count(seq_, POLLIN);
        if (npfds <= 0) break;
        std::vector<struct pollfd> pfds((std::size_t) npfds);
        snd_seq_poll_descriptors(seq_, pfds.data(), (unsigned) npfds, POLLIN);
        if (poll(pfds.data(), (nfds_t) npfds, 50) <= 0) continue;

        snd_seq_event_t *ev = nullptr;
        while (snd_seq_event_input(seq_, &ev) >= 0 && ev) {
            std::uint8_t buf[64];
            long n = 0;
            if (parser_) {
                snd_midi_event_reset_decode(parser_);
                n = snd_midi_event_decode(parser_, buf, (long) sizeof(buf), ev);
            }
            if (n > 0) {
                MidiInputCallback cb;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    cb = cb_;
                }
                if (cb) cb(buf, (std::size_t) n, MidiOutScheduler::hostNowNs());
            }
            if (snd_seq_event_input_pending(seq_, 0) <= 0) break;
        }
    }
}

}  // namespace audio
