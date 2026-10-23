#ifndef SRESTOREPINNEDCLIPACTION_H
#define SRESTOREPINNEDCLIPACTION_H

#include "app/actions/saction.h"
#include "tw/graph/tw303aenv.h"
#include <QList>

class SRemoveSampleAction;

/**
 * The inverse of deleting a clip whose content nothing can rebuild from
 * parameters (QBX-146 step a, plan/proposed/52).
 *
 * THE NAME SAYS "PINNED", NOT "TAKE COLUMN" (QBX-205). It restores whatever
 * `remove-sample` PINNED, which since QBX-202 is every content that verb is
 * not asked to rebuild from parameters -- a MIDI clip's SMidiSequence above
 * all -- because re-linking the same object needs to know nothing about what
 * the object is. It was called `restore-take-column` until QBX-205, from the
 * one shape it could reach when QBX-146 wrote it, and that name then read as
 * a limit the code no longer had.
 *
 * The rename cost nothing to the outside, which is worth recording so the
 * next person does not go looking for the compatibility seam: this action is
 * LIVE-ONLY and registered NOWHERE, so the string reaches no `.qxa`, no saved
 * project and no golden, and `action_roundtrip_test` (which enumerates the
 * registry) never sees it. It is constructed in exactly one place,
 * `SRemoveSampleAction::apply`, and read only by logs.
 *
 * `restore-pinned-clip` is deliberately parallel to `restore-container-clip`,
 * because the pair is the real distinction: this one restores the SAME object
 * from a pin, that one REBUILDS a clip from a container recipe.
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
class SRestorePinnedClipAction : public SAction {
public:
    SRestorePinnedClipAction() = default;
    SRestorePinnedClipAction( SRemoveSampleAction *owner,
                              const QList<int> &lanePath,
                              offset_t timePos,
                              int restoreAtIndex );

    QString name() const override
        { return QStringLiteral("restore-pinned-clip"); }
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

#endif // SRESTOREPINNEDCLIPACTION_H
