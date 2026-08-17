#ifndef _SMIDIINPUTHUB_H_
#define _SMIDIINPUTHUB_H_

#include <map>
#include <memory>
#include <vector>

#include <QString>
#include <QStringList>

#include "tw/devices/midi_in_fanout.h"
#include "tw/devices/midi_input.h"
#include "tw/devices/midi_output.h"

/**
 * SMidiInputHub - the app's ONE owner of open MIDI INPUT ports (proposal 21
 * L2, design D8/D9).
 *
 * A port is opened once, by its PORTABLE NAME, and kept for the process. Every
 * consumer that wants what it delivers acquires a SINK from that port's
 * `MidiInFanout` - its own SPSC ring, filled at the device callback - rather
 * than a second callback or a shared queue:
 *
 *   - the LIVE LANE, one sink per consuming instrument (L2, this phase);
 *   - the MIDI RECORDER, one sink (L4 - `recorderSink()` is the hook, and it
 *     is deliberately spelled out here so the shape is fixed before there is a
 *     second consumer to argue with);
 *   - MIDI-THRU, which is not a sink at all: the device thread pushes straight
 *     into MidiOutScheduler's immediate ring.
 *
 * WHY A PORT IS NEVER CLOSED once opened, until teardown. Two reasons, and the
 * second is the load-bearing one. Opening a MIDI device is not free and a
 * disarm/arm cycle must not drop it (the same argument SMidiOutPump makes for
 * its schedulers). And `CaptureMidiInput::inject()` - which is how every
 * headless case feeds MIDI - is a no-op on a CLOSED port, so closing one on
 * disarm would silently swallow a script's events between two phases of a case.
 *
 * THE KEYBOARD is a port like any other (design D9): `createMidiInput
 * ("keyboard")`, named `keyboard`, enumerated beside the hardware ports. It is
 * NOT reachable through SMARAGD_MIDI_BACKEND - that variable chooses the
 * system MIDI implementation, and the computer keyboard is present whatever it
 * chooses.
 *
 * THREADING. Everything here is the main thread. The device threads touch only
 * the fanouts' rings and the scheduler's immediate ring.
 */
class SMidiInputHub
{
public:
    SMidiInputHub();
    ~SMidiInputHub();

    SMidiInputHub( const SMidiInputHub & )            = delete;
    SMidiInputHub &operator=( const SMidiInputHub & ) = delete;

    /**
     * The fan-out of the port called `portName`, opening it if needed. An
     * EMPTY name is the default system input port; `keyboard` is the
     * in-process one. Null when the port will not open (logged once).
     */
    audio::MidiInFanout *fanoutFor( const QString &portName );

    /** Is this port open right now? (diagnostics / describe()) */
    bool isOpen( const QString &portName ) const;

    /**
     * The MIDI RECORDER's sink on `portName` (proposal 21 L4). One per port,
     * minted on first ask and kept: a recorder that acquired and released per
     * pass would lose whatever arrived between passes, which is exactly the
     * retrospective-capture buffer L4 wants to keep.
     */
    audio::MidiInFanout::Sink *recorderSink( const QString &portName );

    /** Every input port this machine offers, keyboard first. */
    std::vector<audio::MidiPortInfo> listPorts() const;

    /**
     * A portable port NAME -> the machine-local id `open()` wants. An explicit
     * `SSettings` mapping first, then a case-insensitive match against the
     * backend's own id/name list, then the name verbatim - the same three
     * steps `SMidiOutPump::resolvePortId` takes for outputs.
     */
    QString resolvePortId( const QString &portName ) const;

    /** `port=<name> open=<0|1> msgs=<n> thru=<n>` per open port. */
    QString describe() const;

private:
    struct Port {
        std::unique_ptr<audio::MidiInput>    input;
        std::unique_ptr<audio::MidiInFanout> fanout;
        audio::MidiInFanout::Sink           *recorder = nullptr;
        QString                              deviceId;
    };

    Port *portFor( const QString &portName );

    std::map<QString, std::unique_ptr<Port> > ports_;
    // The enumeration probes, constructed once. Never opened: a probe that
    // opened a device would take it away from the port that wants to listen.
    std::unique_ptr<audio::MidiInput> probe_;
    QStringList                       failed_;
};

#endif // _SMIDIINPUTHUB_H_
