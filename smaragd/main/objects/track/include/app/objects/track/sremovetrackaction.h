#ifndef SREMOVETRACKACTION_H
#define SREMOVETRACKACTION_H

#include "app/actions/saction.h"

class STrack;

// Action: remove a top-level track (and its whole subtree) from the mixer.
//
// Undoable: rather than letting the removed track die, apply() pins it with an
// extra reference held *on this action object* — which keeps the track and its
// entire child subtree alive and intact. The inverse (SRestoreTrackAction) reads
// that pinned track back and re-inserts it at its original index, preserving
// object identity across undo/redo. The pin lives on the persistent forward
// action (the undo command reuses it), not on the transient inverse the harness
// deletes after each undo/redo.
class SRemoveTrackAction : public SAction {
public:
    SRemoveTrackAction() = default;
    explicit SRemoveTrackAction(int index);
    ~SRemoveTrackAction() override;

    QString name() const override { return QStringLiteral("remove-track"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

    // Used by SRestoreTrackAction to read back and release the pinned track.
    STrack *heldTrack() const { return heldTrack_; }
    void releaseHeld();        // drop the pin (after the track is re-attached)

private:
    void dropStalePin();       // release a pin left from a previous apply

    int     index_;
    STrack *heldTrack_ = nullptr;   // the removed track, kept alive by holdsRef_
    bool    holdsRef_ = false;
};

#endif // SREMOVETRACKACTION_H
