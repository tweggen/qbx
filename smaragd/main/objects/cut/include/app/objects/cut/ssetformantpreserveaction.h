#ifndef SSETFORMANTPRESERVEACTION_H
#define SSETFORMANTPRESERVEACTION_H

#include "app/actions/saction.h"
#include <QList>

// Action: set a clip's formant-preservation flag (proposal 28 W4). ON keeps
// the spectral envelope in place while the vocoder's pitch stage moves the
// harmonics — the opt-in for vocal material; OFF (the default) is the
// pre-W4 behavior, byte-identical output. The value is ABSOLUTE (undo
// restores the exact previous flag). Take stacks: a PER-TAKE parameter,
// same rule as pitch (take -1 = the active take, resolved at apply time).
class SSetFormantPreserveAction : public SAction {
public:
    SSetFormantPreserveAction() = default;
    SSetFormantPreserveAction( const QList<int> &clipPath, bool on,
                               int take = -1, bool broadcast = true );

    QString name() const override
        { return QStringLiteral("set-formant-preserve"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> clipPath_;
    bool       on_        = false;
    int        take_      = -1;
    bool       broadcast_ = true;
};

#endif // SSETFORMANTPRESERVEACTION_H
