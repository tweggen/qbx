#ifndef SREMOVETAKEACTION_H
#define SREMOVETAKEACTION_H

#include "app/actions/saction.h"
#include "tw/events/twcompmap.h"
#include <QList>

class SClipWindow;

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
// same index. The extent is carried rather than re-derived: an `add-take` with
// no `at`/`length` means the whole column, so an inverse without them would
// hand back a take covering more than the one it replaced.
//
// ...AND A REAL INVERSE FOR A TAKE WITH NO FILE BEHIND IT (plan 53 P6,
// QBX-184). This used to be a `nullptr` inverse, i.e. a removal that could not
// be undone at all -- the hole plan 52 §6 named. It mattered the moment
// `cleanup-takes` arrived: an EVENT take has no file path, so a clean-up that
// removed one could not give it back, and the one thing a destructive sweep
// must be is undoable.
//
// THE PIN PATTERN closes it, exactly as `SRemoveTrackAction` closes the same
// shape for a track: `addRef()` the removed window so the refcount release in
// `STakeStack::removeTake` cannot delete it, keep it on THIS action (the
// persistent forward one the undo command reuses, never the transient inverse
// the harness deletes after each undo/redo), and let `SRestoreTakeAction` put
// that very object back at its index with its extent. Object identity survives,
// which for an event take is the whole point: its notes are its own children,
// and nothing could rebuild them from a path.
//
// A FILE-BACKED take keeps the `add-take` inverse rather than the pin, and the
// difference is not laziness: that inverse is a real `add-take` with a path, so
// it SERIALIZES and reads back in a .qxa, while a pin exists only in this
// process. Where both work, the one that survives a save is the better one.
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

    ~SRemoveTakeAction() override;

    // Read back and released by SRestoreTakeAction, the SRemoveTrackAction /
    // SRestoreTrackAction pair's own protocol.
    SClipWindow *heldTake() const { return heldTake_; }
    const twCompMap &heldCompMap() const { return restoreMap_; }
    bool hasHeldCompMap() const { return hasRestoreMap_; }
    int  heldActiveTake() const { return heldActive_; }
    void releaseHeld();

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

    void dropStalePin();           // release a pin left from a previous apply

    SClipWindow *heldTake_ = nullptr;   // the removed window, kept alive by the pin
    bool         holdsRef_ = false;
    int          heldActive_ = -1;      // the column's activeTake_ before the removal
};

#endif // SREMOVETAKEACTION_H
