#ifndef _SASSERTRETURNTRACKACTION_H_
#define _SASSERTRETURNTRACKACTION_H_

#include "app/actions/saction.h"
#include <QString>

/**
 * `assert-return-track` -- a RETURN TRACK's model AND its live wiring
 * (proposal 55 M2, §3.2).
 *
 * It reads the LIVE plugs, not only the attribute, for 47's
 * assert-master-inputs lesson: a wired and a wiped route are both silent on a
 * quiet fixture, so only the wiring can say which one a pass left behind.
 *
 *   trackPath = ""   the return track (any lane spelling)
 *   isReturn  = ""   "1"/"0": is the aux-source attribute set
 *   source    = ""   the expected source track (any lane spelling)
 *   slot      = -1   the expected source slot INDEX, found from the stored id
 *   bus       = -1   the expected bus
 *   routed    = ""   "1"/"0": the tap is this chain's input AND reads the
 *                    source insert (STrack::returnRouted)
 *   mapping   = ""   a substring of the tap's describe(), e.g. "mapping=copy"
 *   mask      = -1   the routed mask the SOURCE slot was handed by the pass
 */
class SAssertReturnTrackAction : public SAction
{
public:
    SAssertReturnTrackAction() {}
    SApplyResult apply( SProject *project ) override;
    QString name() const override { return QStringLiteral( "assert-return-track" ); }
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override
    { return { QStringLiteral( "trackPath" ), QStringLiteral( "isReturn" ),
               QStringLiteral( "source" ), QStringLiteral( "slot" ),
               QStringLiteral( "bus" ), QStringLiteral( "routed" ),
               QStringLiteral( "mapping" ), QStringLiteral( "mask" ) }; }

private:
    QString trackPath_;
    QString isReturn_;
    QString source_;
    int     slot_ = -1;
    int     bus_  = -1;
    QString routed_;
    QString mapping_;
    int     mask_ = -1;
};

#endif
