#ifndef _SASSERTPLUGINSIDECHAINACTION_H_
#define _SASSERTPLUGINSIDECHAINACTION_H_

#include "app/actions/saction.h"
#include <QString>

/**
 * `assert-plugin-sidechain` -- a KEYED SLOT's model AND its live wiring
 * (proposal 55 M4, §3.2).
 *
 * It reads the LIVE plug, not only the attribute, for 47's
 * assert-master-inputs lesson: a wired and a wiped key both leave a quiet
 * fixture quiet, so only the wiring can say which one a pass left behind.
 *
 *   trackPath = ""   the keyed track (any lane spelling)
 *   slot      = 0    the slot index
 *   keyed     = ""   "1"/"0": does the slot carry a sidechain at all
 *   source    = ""   the expected key lane (any lane spelling)
 *   tap       = ""   "pre"/"post"
 *   wired     = ""   "1"/"0": port 1 of the slot's insert reads the key lane's
 *                    tap component (STrack::pluginSlotKeyWired)
 *   mapping   = ""   a substring of the processor's describeKey(), e.g.
 *                    "mapping=copy"
 */
class SAssertPluginSidechainAction : public SAction
{
public:
    SAssertPluginSidechainAction() {}
    SApplyResult apply( SProject *project ) override;
    QString name() const override { return QStringLiteral( "assert-plugin-sidechain" ); }
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override
    { return { QStringLiteral( "trackPath" ), QStringLiteral( "slot" ),
               QStringLiteral( "keyed" ), QStringLiteral( "source" ),
               QStringLiteral( "tap" ), QStringLiteral( "wired" ),
               QStringLiteral( "mapping" ) }; }

private:
    QString trackPath_;
    int     slot_ = 0;
    QString keyed_;
    QString source_;
    QString tap_;
    QString wired_;
    QString mapping_;
};

#endif
