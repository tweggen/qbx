#ifndef _SASSERTSENDINPUTSACTION_H_
#define _SASSERTSENDINPUTSACTION_H_

#include "app/actions/saction.h"
#include <QString>

/**
 * `assert-send-inputs` — a SEND BUS's own inputs, read off the live `twMixer`
 * (proposal 47 M4). The send-side twin of `assert-master-inputs`, and it
 * exists for a sharper version of the same reason.
 *
 * **THE ONLY THING THAT CAN GATE D9.** The decision is that a LIVE lane does
 * not feed a send: a live-owned track is excluded from the frozen sum and
 * rendered by the pump, so the gain-stage pages a tap would read are not
 * being produced. That cannot be measured through a render, because
 * `startRender()` SUSPENDS every live lane (21 L1b) — by the time audio
 * exists, the condition under test is gone. It can be measured at the WIRING,
 * which is where the decision lives.
 *
 *   lane   = ""    the send lane, by name (`Reverb`) or sentinel (`$send0`)
 *   count  = -1    expected input count (-1 = not checked)
 *   wired  = -1    how many inputs have a non-null source (-1 = not checked).
 *                  A source that is inaudible, disabled or LIVE-OWNED is
 *                  nulled by the same pass, so this is deliberately separate
 *                  from `count`.
 *   level  =       expected dB on input `index` (needs `index`)
 *   index  = -1    which input `level` refers to
 */
class SAssertSendInputsAction : public SAction
{
public:
    SAssertSendInputsAction() {}
    SApplyResult apply( SProject *project ) override;
    QString name() const override { return QStringLiteral( "assert-send-inputs" ); }
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override
    { return { QStringLiteral( "lane" ), QStringLiteral( "count" ),
               QStringLiteral( "wired" ), QStringLiteral( "level" ),
               QStringLiteral( "index" ), QStringLiteral( "arrangement" ) }; }

private:
    QString lane_;
    QString arrangement_;
    int     count_ = -1;
    int     wired_ = -1;
    int     index_ = -1;
    double  level_ = 0.0;
    bool    hasLevel_ = false;
};

#endif
