#ifndef _SASSERTMASTERINPUTSACTION_H_
#define _SASSERTMASTERINPUTSACTION_H_

#include "app/actions/saction.h"
#include <QString>

/**
 * `assert-master-inputs` — the master sum's input count and levels, read off
 * the LIVE twMixer (proposal 45 M7, AC7.4).
 *
 * THE ONLY THING THAT CAN SEE A CLOBBERED SEND LANE. In M7 nothing can feed a
 * send (D10), so a send wired into the master sum and one that was wiped from
 * it BOTH contribute silence — no audio assertion anywhere can separate them.
 * That is the same shape as the missing-sample placeholder, where a dropped
 * clip and a placeholder are equally silent and only a model assertion bites.
 *
 * `reconnectTracksToMixer` sets the input count from the TRACK count and
 * rewires every input on every audibility, solo, mute and arm change (trap
 * T3), so what this verb reads after such a pass is exactly the question AC7.4
 * asks: are the send lanes still there.
 *
 *   arrangement = ""   the root, as add-track's is ("" = the master root)
 *   count       = -1   expected getNInputs() on bus 0 (-1 = not checked)
 *   allUnity    = ""   "1" asserts EVERY input level is exactly 0 dB.
 *                      Not a levelling detail: twlive::checkMasterShape walks
 *                      getNInputs() and refuses the LINEAR master split the
 *                      moment one is not (D4a rule 2), so a send at anything
 *                      else drops live monitoring into the Closure path for
 *                      every armed track in the project.
 *   wired       = -1   how many inputs have a non-null source (-1 = not
 *                      checked). A muted or soloed-out track is nulled by the
 *                      same pass, so this is deliberately separate from
 *                      `count`.
 */
class SAssertMasterInputsAction : public SAction
{
public:
    SAssertMasterInputsAction() {}
    SApplyResult apply( SProject *project ) override;
    QString name() const override
    { return QStringLiteral( "assert-master-inputs" ); }
    QStringList knownAttributes() const override
    { return { QStringLiteral( "arrangement" ), QStringLiteral( "count" ),
               QStringLiteral( "allUnity" ),    QStringLiteral( "wired" ) }; }
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString arrangement_;
    int     count_ = -1;
    QString allUnity_;
    int     wired_ = -1;
};

#endif // _SASSERTMASTERINPUTSACTION_H_
