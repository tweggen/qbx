#ifndef SREMOVETRACKACTION_H
#define SREMOVETRACKACTION_H

#include "app/actions/saction.h"
#include <QList>

class STrack;

// Action: remove a track (and its whole subtree) from its container.
//
// The track is addressed by an index-PATH from the root mixer ({2} = the 3rd
// top-level track, {0,1} = the 2nd child of the 1st). It used to be a single
// top-level index resolved with SStdMixer::getTrackAt(), which meant a track
// NESTED inside a folder could not be named — and the arranger's "Remove track"
// menu item accordingly returned without submitting anything, silently. The
// legacy `index` attribute is still ACCEPTED on read.
//
// The container may therefore be the root mixer OR a folder STrack, and the two
// detach differently: the mixer owns its track list (removeTrack rewires the
// summing inputs), while a folder holds an ordinary SLink child that is simply
// deleted. Mirrors SReparentTrackAction, which has always had to handle both.
//
// Undoable: rather than letting the removed track die, apply() pins it with an
// extra reference held *on this action object* — which keeps the track and its
// entire child subtree alive and intact. The inverse (SRestoreTrackAction) reads
// that pinned track back and re-inserts it under its original parent at its
// original index, preserving object identity across undo/redo. The pin lives on
// the persistent forward action (the undo command reuses it), not on the
// transient inverse the harness deletes after each undo/redo.
class SRemoveTrackAction : public SAction {
public:
    SRemoveTrackAction() = default;
    explicit SRemoveTrackAction(const QList<int> &trackPath);
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

    QList<int> trackPath_;
    STrack *heldTrack_ = nullptr;   // the removed track, kept alive by holdsRef_
    bool    holdsRef_ = false;
};

#endif // SREMOVETRACKACTION_H
