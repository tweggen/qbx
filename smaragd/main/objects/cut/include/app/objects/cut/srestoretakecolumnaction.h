#ifndef SRESTORETAKECOLUMNACTION_H
#define SRESTORETAKECOLUMNACTION_H

#include "app/actions/saction.h"
#include "tw/graph/tw303aenv.h"
#include <QList>

class SRemoveSampleAction;

/**
 * The inverse of deleting a clip whose content nothing can rebuild from
 * parameters -- a take column (QBX-146 step a, plan/proposed/52).
 *
 * It restores the clip by RE-LINKING THE VERY OBJECT that was deleted, which
 * SRemoveSampleAction kept alive with an addRef() pin. That is the whole point:
 * a column carries N takes with their own windows, fades, names, pan and
 * automation, an active take index, and a comp map -- and on the wrapped shape
 * a wrapper whose own automation, volume, fade and pan are what the mix reads.
 * Rebuilding that from a captured parameter list drops whatever the list
 * forgot, which is exactly how SAddSampleAction once restored "a full-length
 * unedited clip... silently, with no dialog to notice". Identity preserves
 * every field, including ones nobody has thought of yet.
 *
 * The pattern is SRemoveTrackAction / SRestoreTrackAction: the forward holds
 * the pin, this borrows it through a back-pointer, re-links, and releases. The
 * back-pointer is safe for the undo command's whole life -- the command owns
 * both halves and deletes them together.
 *
 * LIVE-ONLY. It carries a pointer to a live action and a live SObject, which no
 * XML can express; writeXml is empty and readXml returns false, exactly as
 * SRestoreContainerClipAction is. The FORWARD stays serializable, just as
 * remove-track does while restore-track does not.
 */
class SRestoreTakeColumnAction : public SAction {
public:
    SRestoreTakeColumnAction() = default;
    SRestoreTakeColumnAction( SRemoveSampleAction *owner,
                              const QList<int> &lanePath,
                              offset_t timePos,
                              int restoreAtIndex );

    QString name() const override
        { return QStringLiteral("restore-take-column"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    SRemoveSampleAction *owner_ = nullptr;   // holds the pin; borrowed
    QList<int> lanePath_;
    offset_t   timePos_ = 0;
    // Where in the lane's child order the clip came from (QBX-149): an inverse
    // owes child order back, or a redo of the delete resolves the same
    // positional index to a different clip.
    int        restoreAtIndex_ = -1;
};

#endif // SRESTORETAKECOLUMNACTION_H
