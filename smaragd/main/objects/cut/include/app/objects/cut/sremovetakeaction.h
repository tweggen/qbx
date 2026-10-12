#ifndef SREMOVETAKEACTION_H
#define SREMOVETAKEACTION_H

#include "app/actions/saction.h"
#include "tw/events/twcompmap.h"
#include <QList>

// Action: remove one take from a take stack (proposal 17). When a single
// FULL-EXTENT take remains afterwards, the stack collapses back to a plain cut
// placement (invariant 3′; a surviving PARTIAL take stays a column, plan 53
// §4.2). Removing the active take leaves no take audible unless `thenActivate`
// (>= -1) explicitly selects one afterwards — the add-take inverse uses this
// to restore the pre-add selection exactly.
//
// THE COLUMN DOES NOT SHRINK. Since invariant 1′ the column has a length of
// its own, so removing the take that reached furthest leaves a span nothing
// covers — which plays SILENCE rather than moving the clip's right edge under
// the user.
//
// Inverse: add-take with the removed take's file/window AND ITS EXTENT at the
// same index (null — not undoable — when the take's content has no file
// path). The extent is carried rather than re-derived: an `add-take` with no
// `at`/`length` means the whole column, so an inverse without them would hand
// back a take covering more than the one it replaced.
class SRemoveTakeAction : public SAction {
public:
    SRemoveTakeAction() = default;
    SRemoveTakeAction( const QList<int> &clipPath, int takeIndex,
                       int thenActivate = -2 );

    /**
     * Restore THIS comp map after the removal (plan 53 B4).
     *
     * An inverse-only payload, set by the action that built this one; a
     * scripted `remove-take` never carries it. It exists because `add-take
     * activate=1` must CLEAR the map -- otherwise the newly activated take is
     * badged and inaudible -- and the map is not derivable from the surviving
     * takes, so only the forward action can hand it back.
     *
     * Not serialized: like SAddSampleAction's `restoreAtIndex`, it is read by
     * the undo stack in this process and never written to a .qxa.
     */
    void setRestoreCompMap( const twCompMap &map )
    { restoreMap_ = map; hasRestoreMap_ = true; }

    QString name() const override { return QStringLiteral("remove-take"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> clipPath_;
    int        takeIndex_ = 0;
    int        thenActivate_ = -2;   // -2 = keep the bookkeeping result
    twCompMap  restoreMap_;
    bool       hasRestoreMap_ = false;
};

#endif // SREMOVETAKEACTION_H
