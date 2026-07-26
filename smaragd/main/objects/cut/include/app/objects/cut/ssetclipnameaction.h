#ifndef SSETCLIPNAMEACTION_H
#define SSETCLIPNAMEACTION_H

#include "app/actions/saction.h"
#include <QList>
#include <QString>

// Action: rename a clip (the SObject SName the timeline draws on the clip
// body). The value is ABSOLUTE — undo restores the exact previous string,
// not a delta — because a name has no meaningful increment. Take stacks:
// a PER-TAKE property, same rule as pitch/formant (take -1 = the active
// take, resolved at apply time so the inverse can name it explicitly).
class SSetClipNameAction : public SAction {
public:
    SSetClipNameAction() = default;
    SSetClipNameAction( const QList<int> &clipPath, const QString &name,
                        int take = -1, bool broadcast = true );

    QString name() const override
        { return QStringLiteral("set-clip-name"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> clipPath_;
    QString    name_;
    int        take_      = -1;
    bool       broadcast_ = true;
};

#endif // SSETCLIPNAMEACTION_H
