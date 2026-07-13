#ifndef SREMOVETAKEACTION_H
#define SREMOVETAKEACTION_H

#include "app/actions/saction.h"
#include <QList>

// Action: remove one take from a take stack (proposal 17). When a single
// take remains afterwards, the stack collapses back to a plain cut
// placement (invariant 3). Removing the active take leaves no take audible
// unless `thenActivate` (>= -1) explicitly selects one afterwards — the
// add-take inverse uses this to restore the pre-add selection exactly.
//
// Inverse: add-take with the removed take's file/window at the same index
// (null — not undoable — when the take's content has no file path).
class SRemoveTakeAction : public SAction {
public:
    SRemoveTakeAction() = default;
    SRemoveTakeAction( const QList<int> &clipPath, int takeIndex,
                       int thenActivate = -2 );

    QString name() const override { return QStringLiteral("remove-take"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> clipPath_;
    int        takeIndex_ = 0;
    int        thenActivate_ = -2;   // -2 = keep the bookkeeping result
};

#endif // SREMOVETAKEACTION_H
