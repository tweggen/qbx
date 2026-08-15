#include "coremidi_midi.h"

#include "tw/devices/midi_out_scheduler.h"   // hostNowNs
#include "tw/core/twsyslog.h"

#include <CoreAudio/HostTime.h>
#include <CoreFoundation/CoreFoundation.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace audio {

namespace {

std::string cfToStd(CFStringRef s)
{
    if (!s) return {};
    char buf[256] = { 0 };
    if (!CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8)) return {};
    return std::string(buf);
}

std::string endpointName(MIDIEndpointRef ep)
{
    CFStringRef name = nullptr;
    if (MIDIObjectGetStringProperty(ep, kMIDIPropertyDisplayName, &name) != noErr || !name)
        return {};
    const std::string s = cfToStd(name);
    CFRelease(name);
    return s;
}

std::string endpointId(MIDIEndpointRef ep)
{
    SInt32 uid = 0;
    if (MIDIObjectGetIntegerProperty(ep, kMIDIPropertyUniqueID, &uid) != noErr) return {};
    return std::to_string((long) uid);
}

CFStringRef cfName(const std::string &s)
{
    return CFStringCreateWithCString(kCFAllocatorDefault, s.c_str(), kCFStringEncodingUTF8);
}

// A MIDIPacket timestamp is a MACH host time. hostNowNs() is steady_clock,
// which on macOS is also mach-based — but rather than rely on the two epochs
// being identical, everything goes through a DELTA against "now" in each clock.
// That is correct whatever the epochs are, and costs one extra read.
MIDITimeStamp toHostTime(std::int64_t dueHostTimeNs)
{
    if (dueHostTimeNs == 0) return 0;                    // 0 == "as soon as possible"
    const std::int64_t deltaNs = dueHostTimeNs - MidiOutScheduler::hostNowNs();
    if (deltaNs <= 0) return 0;
    return AudioGetCurrentHostTime() + AudioConvertNanosToHostTime((UInt64) deltaNs);
}

}  // namespace

// --- output -----------------------------------------------------------------

CoreMidiOutput::~CoreMidiOutput()
{
    close();
}

bool CoreMidiOutput::ensureClient()
{
    if (client_) return true;
    CFStringRef n = cfName("Smaragd");
    const OSStatus r = MIDIClientCreate(n, nullptr, nullptr, &client_);
    CFRelease(n);
    if (r != noErr) {
        syslog(LOG_WARNING, "midi: MIDIClientCreate failed with %d", (int) r);
        client_ = 0;
        return false;
    }
    return true;
}

int CoreMidiOutput::open(const std::string &portId)
{
    close();
    if (!ensureClient()) return -1;

    const ItemCount n = MIDIGetNumberOfDestinations();
    if (n == 0) {
        syslog(LOG_INFO, "midi: no CoreMIDI destinations present");
        return -1;
    }

    MIDIEndpointRef chosen = 0;
    if (portId.empty() || portId == "default") {
        chosen = MIDIGetDestination(0);
    } else {
        for (ItemCount i = 0; i < n; ++i) {
            MIDIEndpointRef ep = MIDIGetDestination(i);
            if (endpointId(ep) == portId) { chosen = ep; break; }
        }
    }
    if (!chosen) {
        syslog(LOG_WARNING, "midi: CoreMIDI has no destination '%s'", portId.c_str());
        return -1;
    }

    CFStringRef pn = cfName("Smaragd Out");
    const OSStatus r = MIDIOutputPortCreate(client_, pn, &port_);
    CFRelease(pn);
    if (r != noErr) {
        syslog(LOG_WARNING, "midi: MIDIOutputPortCreate failed with %d", (int) r);
        port_ = 0;
        return -1;
    }
    dest_ = chosen;
    return 0;
}

int CoreMidiOutput::close()
{
    if (port_)       { MIDIPortDispose(port_);       port_ = 0; }
    if (virtualSrc_) { MIDIEndpointDispose(virtualSrc_); virtualSrc_ = 0; }
    if (client_)     { MIDIClientDispose(client_);   client_ = 0; }
    dest_ = 0;
    return 0;
}

std::vector<MidiPortInfo> CoreMidiOutput::listPorts() const
{
    std::vector<MidiPortInfo> ports;
    const ItemCount n = MIDIGetNumberOfDestinations();
    for (ItemCount i = 0; i < n; ++i) {
        MIDIEndpointRef ep = MIDIGetDestination(i);
        std::string id = endpointId(ep);
        if (id.empty()) id = std::to_string((long) i);
        ports.push_back(MidiPortInfo{ id, endpointName(ep), false });
    }
    return ports;
}

bool CoreMidiOutput::createVirtualPort(const std::string &name)
{
    if (!ensureClient()) return false;
    if (virtualSrc_) return true;

    CFStringRef n = cfName(name.empty() ? std::string("Smaragd") : name);
    const OSStatus r = MIDISourceCreate(client_, n, &virtualSrc_);
    CFRelease(n);
    if (r != noErr) {
        syslog(LOG_WARNING, "midi: MIDISourceCreate failed with %d", (int) r);
        virtualSrc_ = 0;
        return false;
    }
    return true;
}

int CoreMidiOutput::send(const std::uint8_t *bytes, std::size_t size,
                         std::int64_t hostTimeNs)
{
    if (!bytes || size == 0) return -1;
    if (!dest_ && !virtualSrc_) return -1;

    // 1 KB is far more than the 16-byte ring slot can ever hold; it also leaves
    // room for the sysex path a later phase may add.
    Byte storage[1024];
    MIDIPacketList *list = reinterpret_cast<MIDIPacketList *>(storage);
    MIDIPacket     *pkt  = MIDIPacketListInit(list);
    pkt = MIDIPacketListAdd(list, sizeof(storage), pkt, toHostTime(hostTimeNs),
                            size, bytes);
    if (!pkt) return -1;

    OSStatus r = noErr;
    if (dest_) r = MIDISend(port_, dest_, list);
    // A virtual source publishes to whoever connected to us; both can be live.
    if (virtualSrc_) {
        const OSStatus r2 = MIDIReceived(virtualSrc_, list);
        if (r == noErr) r = r2;
    }
    return r == noErr ? 0 : -1;
}

// --- input ------------------------------------------------------------------

CoreMidiInput::~CoreMidiInput()
{
    close();
}

bool CoreMidiInput::ensureClient()
{
    if (client_) return true;
    CFStringRef n = cfName("Smaragd");
    const OSStatus r = MIDIClientCreate(n, nullptr, nullptr, &client_);
    CFRelease(n);
    if (r != noErr) {
        syslog(LOG_WARNING, "midi: MIDIClientCreate failed with %d", (int) r);
        client_ = 0;
        return false;
    }
    return true;
}

int CoreMidiInput::open(const std::string &portId)
{
    close();
    if (!ensureClient()) return -1;

    const ItemCount n = MIDIGetNumberOfSources();
    if (n == 0) {
        syslog(LOG_INFO, "midi: no CoreMIDI sources present");
        return -1;
    }

    MIDIEndpointRef chosen = 0;
    if (portId.empty() || portId == "default") {
        chosen = MIDIGetSource(0);
    } else {
        for (ItemCount i = 0; i < n; ++i) {
            MIDIEndpointRef ep = MIDIGetSource(i);
            if (endpointId(ep) == portId) { chosen = ep; break; }
        }
    }
    if (!chosen) {
        syslog(LOG_WARNING, "midi: CoreMIDI has no source '%s'", portId.c_str());
        return -1;
    }

    CFStringRef pn = cfName("Smaragd In");
    const OSStatus r = MIDIInputPortCreate(client_, pn, &CoreMidiInput::readProc, this,
                                           &port_);
    CFRelease(pn);
    if (r != noErr) {
        syslog(LOG_WARNING, "midi: MIDIInputPortCreate failed with %d", (int) r);
        port_ = 0;
        return -1;
    }
    if (MIDIPortConnectSource(port_, chosen, nullptr) != noErr) {
        MIDIPortDispose(port_);
        port_ = 0;
        return -1;
    }
    source_ = chosen;
    return 0;
}

int CoreMidiInput::close()
{
    if (port_ && source_) MIDIPortDisconnectSource(port_, source_);
    if (port_)        { MIDIPortDispose(port_);            port_ = 0; }
    if (virtualDest_) { MIDIEndpointDispose(virtualDest_); virtualDest_ = 0; }
    if (client_)      { MIDIClientDispose(client_);        client_ = 0; }
    source_ = 0;
    return 0;
}

std::vector<MidiPortInfo> CoreMidiInput::listPorts() const
{
    std::vector<MidiPortInfo> ports;
    const ItemCount n = MIDIGetNumberOfSources();
    for (ItemCount i = 0; i < n; ++i) {
        MIDIEndpointRef ep = MIDIGetSource(i);
        std::string id = endpointId(ep);
        if (id.empty()) id = std::to_string((long) i);
        ports.push_back(MidiPortInfo{ id, endpointName(ep), false });
    }
    return ports;
}

bool CoreMidiInput::createVirtualPort(const std::string &name)
{
    if (!ensureClient()) return false;
    if (virtualDest_) return true;

    CFStringRef n = cfName(name.empty() ? std::string("Smaragd") : name);
    const OSStatus r = MIDIDestinationCreate(client_, n, &CoreMidiInput::readProc, this,
                                             &virtualDest_);
    CFRelease(n);
    if (r != noErr) {
        syslog(LOG_WARNING, "midi: MIDIDestinationCreate failed with %d", (int) r);
        virtualDest_ = 0;
        return false;
    }
    return true;
}

void CoreMidiInput::setCallback(MidiInputCallback cb)
{
    std::lock_guard<std::mutex> lock(mutex_);
    cb_ = std::move(cb);
}

void CoreMidiInput::readProc(const MIDIPacketList *pkts, void *readRefCon,
                             void * /*srcRefCon*/)
{
    auto *self = reinterpret_cast<CoreMidiInput *>(readRefCon);
    if (self) self->deliver(pkts);
}

void CoreMidiInput::deliver(const MIDIPacketList *pkts)
{
    if (!pkts) return;

    MidiInputCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = cb_;
    }
    if (!cb) return;

    // The read proc runs on CoreMIDI's own high-priority thread: no Qt, no
    // blocking, and the callback is invoked with no lock of ours held.
    const std::int64_t nowNs   = MidiOutScheduler::hostNowNs();
    const UInt64       nowHost = AudioGetCurrentHostTime();

    const MIDIPacket *p = &pkts->packet[0];
    for (UInt32 i = 0; i < pkts->numPackets; ++i) {
        const std::int64_t deltaNs =
            p->timeStamp == 0
                ? 0
                : (std::int64_t) AudioConvertHostTimeToNanos(p->timeStamp) -
                      (std::int64_t) AudioConvertHostTimeToNanos(nowHost);
        cb(p->data, p->length, nowNs + deltaNs);
        p = MIDIPacketNext(p);
    }
}

}  // namespace audio
