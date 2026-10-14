#ifndef SRESTORETAKEACTION_H
#define SRESTORETAKEACTION_H

#include "app/actions/saction.h"
#include "tw/core/twtypes.h"

#include <QList>

class SRemoveTakeAction;

/**
 * Inverse of `SRemoveTakeAction` for a take WITH NO FILE BEHIND IT (plan 53
 * P6, QBX-184). It re-inserts the window its owner is holding alive, at the
 * index and with the extent it had, restoring object IDENTITY rather than
 * rebuilding from a recipe.
 *
 * WHY IT HAS TO EXIST. `remove-take`'s ordinary inverse is an `add-take` with
 * the removed take's file path, and for a take whose content is not a file
 * there is no path to give it -- so the inverse used to be `nullptr`, i.e. the
 * removal simply could not be undone. That is the hole plan 52 §6 names, and
 * it stopped being theoretical when `cleanup-takes` arrived: an EVENT take has
 * no file, so a clean-up that removed one could not give it back, and the one
 * thing a destructive sweep must be is undoable. An event take's notes are its
 * own children; nothing could reconstruct them from a path.
 *
 * THE PAIR'S PROTOCOL is `SRemoveTrackAction` / `SRestoreTrackAction`'s,
 * deliberately: the removal `addRef()`s the window so the refcount release in
 * `STakeStack::removeTake` cannot delete it, and keeps it on the PERSISTENT
 * forward action -- the one the undo command reuses -- never on the transient
 * inverse the harness deletes after each undo/redo. This action reads it back
 * through `heldTake()` and releases the pin once the new link owns a reference.
 *
 * It also hands back the ACTIVE TAKE and the COMP MAP as they were. Both are
 * read off the owner rather than copied here: `removeTake` renumbers the map
 * and drops the removed take's segments, and neither step is reversible from
 * the result.
 *
 * LIVE-ONLY, like `SRestoreContainerClipAction`: it is manufactured as an
 * inverse and never appears in a script, so it is neither serialized nor
 * registered. A pin exists only in this process, which is also why a
 * file-backed take keeps the `add-take` inverse -- that one survives a save.
 */
class SRestoreTakeAction : public SAction {
public:
    SRestoreTakeAction( SRemoveTakeAction *owner, const QList<int> &clipPath,
                        int takeIndex, bool wasActive, offset_t extentStart,
                        length_t extentLen );

    QString name() const override
    { return QStringLiteral( "restore-take" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    SRemoveTakeAction *owner_;      // holds the pinned window (not owned)
    QList<int> clipPath_;
    int        takeIndex_   = 0;
    bool       wasActive_   = false;
    offset_t   extentStart_ = 0;
    length_t   extentLen_   = -1;
};

#endif // SRESTORETAKEACTION_H
