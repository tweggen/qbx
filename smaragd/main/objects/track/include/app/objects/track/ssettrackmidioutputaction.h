#ifndef _SSETTRACKMIDIOUTPUTACTION_H_
#define _SSETTRACKMIDIOUTPUTACTION_H_

#include <QList>
#include <QString>

#include "app/actions/saction.h"

/**
 * `set-track-midi-output` - where this track's event FEED goes on the wire
 * (proposal 36 D6, 3.2 / 3.4). ABSOLUTE, like every other track flag: it
 * carries the whole state, and its inverse carries the previous whole state.
 *
 *   port      - a PORTABLE device NAME, never a machine-local id. WinMM device
 *               indices and CoreMIDI uniqueIDs mean nothing on the next
 *               machine, so the project stores the name and `SSettings`
 *               resolves it per machine (the same split the audio output
 *               device selection uses). "" = no MIDI output.
 *   channel   - 0-BASED, 0..15, matching `twEvent::channel` and `add-note
 *               channel=`, so the scripting API speaks ONE convention.
 *               -1 = "as authored": every event keeps its own channel.
 *   offsetMs  - signed per-track send offset, +-500 ms, POSITIVE = send
 *               EARLIER. The requester's outboard-gear case: gear whose audio
 *               return arrives late is compensated so its audio lands on the
 *               grid. A global default lives in Options -> MIDI and is added
 *               to this one by the pump.
 *
 * Setting a port also changes the track's `auto` event routing - "consumed
 * here, or bubbled up" - so a child that gains a port stops feeding its
 * parent's instrument. That is why the track range-invalidates on the
 * transition (see STrack::setMidiOutput).
 */
class SSetTrackMidiOutputAction : public SAction
{
public:
    SSetTrackMidiOutputAction() = default;
    SSetTrackMidiOutputAction( const QList<int> &trackPath, const QString &port,
                               int channel, int offsetMs );

    QString name() const override
    { return QStringLiteral( "set-track-midi-output" ); }
    QStringList knownAttributes() const override
    {
        return { QStringLiteral( "trackPath" ), QStringLiteral( "port" ),
                 QStringLiteral( "channel" ), QStringLiteral( "offsetMs" ) };
    }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> trackPath_;
    QString    port_;
    int        channel_  = -1;
    int        offsetMs_ = 0;
};

#endif // _SSETTRACKMIDIOUTPUTACTION_H_
