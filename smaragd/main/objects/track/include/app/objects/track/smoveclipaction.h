#ifndef SMOVECLIPACTION_H
#define SMOVECLIPACTION_H

#include "app/actions/saction.h"
#include "tw/graph/tw303aenv.h"
#include <QList>

// Action: move a clip (an SLink on a track) to a new start time and/or a
// different track — the undoable form of the arranger's clip drag.
//
// The clip is addressed by its track's index-path from the root mixer plus the
// link's index within that track (see strackpath.h + SObject::indexOfChild,
// which keys off the link itself, so it is correct even for shared clips).
// The inverse moves it back to its original track + start; it is synthesized
// from the post-move location, so it survives the index shift the move causes.
//
// Clip order within a track is positional (by start time), so re-attaching by
// append is fine — no exact child-slot bookkeeping is needed.
class SMoveClipAction : public SAction {
public:
    SMoveClipAction() = default;
    SMoveClipAction(const QList<int> &clipPath,
                    const QList<int> &destTrackPath,
                    offset_t newStartTime);

    QString name() const override { return QStringLiteral("move-clip"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    QList<int> clipPath_;        // [track path..., link index in that track]
    QList<int> destTrackPath_;   // path TO the destination track
    offset_t   newStartTime_ = 0;
};

#endif // SMOVECLIPACTION_H
