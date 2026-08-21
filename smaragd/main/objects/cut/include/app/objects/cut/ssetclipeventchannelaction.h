#ifndef SSETCLIPEVENTCHANNELACTION_H
#define SSETCLIPEVENTCHANNELACTION_H

#include "app/actions/saction.h"
#include <QList>

// Action: set the channel remap an SCut placement applies to its content's
// RESIDUAL event feed (proposal 41 D6) — a lane fragment placed on a track
// exports its events on the channel it was authored with unless this
// overrides it, exactly the convention `midiOutChannel` / SMidiCut's own
// channelOverride_ already use.
//
// The value is ABSOLUTE, not a delta (like set-pitch, set-midi-cut): undo
// restores an exact previous value. -1 = as-authored; 0..15 rewrites every
// channel-carrying exported event to that channel.
class SSetClipEventChannelAction : public SAction {
public:
    SSetClipEventChannelAction() = default;
    SSetClipEventChannelAction( const QList<int> &clipPath, int channel );

    QString name() const override { return QStringLiteral("set-clip-event-channel"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> clipPath_;
    int        channel_ = -1;
};

#endif // SSETCLIPEVENTCHANNELACTION_H
