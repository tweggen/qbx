#ifndef SSETFORMANTSHIFTACTION_H
#define SSETFORMANTSHIFTACTION_H

#include "app/actions/saction.h"
#include <QList>

// Action: set a clip's formant-shift offset, in cents, independent of pitch
// and stretch/rate. The vocoder backend warps the spectral envelope by this
// offset regardless of whether the pitch stage runs; the value is ABSOLUTE
// (undo restores the exact previous cents) and clamped to
// SCut::FORMANT_SHIFT_CENTS_LIMIT, same discipline as set-pitch. Take
// stacks: a PER-TAKE parameter, same rule as pitch and formant preservation
// (take -1 = the active take, resolved at apply time).
class SSetFormantShiftAction : public SAction {
public:
    SSetFormantShiftAction() = default;
    SSetFormantShiftAction( const QList<int> &clipPath, double cents,
                            int take = -1, bool broadcast = true );

    QString name() const override
        { return QStringLiteral("set-formant-shift"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> clipPath_;
    double     cents_     = 0.0;
    int        take_      = -1;
    bool       broadcast_ = true;
};

#endif // SSETFORMANTSHIFTACTION_H
