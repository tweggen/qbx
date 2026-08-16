#ifndef SASSERTAUTOMATIONVALUEACTION_H
#define SASSERTAUTOMATIONVALUEACTION_H

#include "app/actions/saction.h"
#include "tw/core/twtypes.h"

#include <QList>
#include <QString>

/**
 * Assertion: what a lane READS at a position (proposal 37 P5, design §3.4).
 *
 * <assert-automation-value owner="0" target="self:Volume"
 *                          time="96000" value="-30" tolerance="1e-6"/>
 *
 * It asks the SNAPSHOT — the same immutable `twAutomationCurve` the engine is
 * handed, through `SAutomationLane::valueAt` — and not a re-implementation of
 * the interpolation, so a case can pin the midpoint of a ramp as a closed form
 * without also having to render it. That is the whole point: the rendered RMS
 * bands tell you the curve reached the audio, this tells you the curve is the
 * curve the script asked for, and a bug that moved BOTH would have to move them
 * consistently.
 *
 * `owner` / `slotIndex` / `take` address the lane exactly as the automation
 * verbs do (see `app/objects/track/sautomationactions.h`): the TARGET's space
 * decides whether `owner` names a lane, a slot's track, or a placement.
 *
 * A missing lane is REJECTED rather than reported as the default value — a typo
 * in a target must not read as a passing assertion. Pair with
 * `expectReject="true"` to assert that a lane is ABSENT.
 */
class SAssertAutomationValueAction : public SAction {
public:
    SAssertAutomationValueAction() = default;

    QString name() const override
    {
        return QStringLiteral( "assert-automation-value" );
    }
    QStringList knownAttributes() const override
    {
        return { QStringLiteral( "owner" ),     QStringLiteral( "target" ),
                 QStringLiteral( "time" ),      QStringLiteral( "value" ),
                 QStringLiteral( "tolerance" ), QStringLiteral( "slotIndex" ),
                 QStringLiteral( "take" ),      QStringLiteral( "mode" ),
                 QStringLiteral( "pointCount" ) };
    }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> ownerPath_;
    QString    target_;
    offset_t   time_ = 0;
    double     value_ = 0.0;
    double     tolerance_ = 1e-6;
    int        slotIndex_ = -1;
    int        take_ = -1;
    QString    mode_;              // optional: the lane's mode must be this
    int        pointCount_ = -1;   // optional: how many points it holds
};

#endif // SASSERTAUTOMATIONVALUEACTION_H
