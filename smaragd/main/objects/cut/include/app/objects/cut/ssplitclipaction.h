#ifndef SSPLITCLIPACTION_H
#define SSPLITCLIPACTION_H

#include "app/actions/saction.h"
#include "tw/graph/tw303aenv.h"
#include <QList>

// Action: split a clip at a timeline position into two clips — the undoable form
// of the arranger's "Split object". The clip is addressed by track-path + link
// index (see strackpath.h). The split point is an absolute timeline offset
// (normally the playhead). If the clip is not already an SCut it is wrapped in
// one first (matching the old behaviour).
//
// Inverse: SUnsplitClipAction, which removes the second part and restores the
// first part's length. (Its own inverse re-splits at the same point.)
class SSplitClipAction : public SAction {
public:
    SSplitClipAction() = default;
    SSplitClipAction(const QList<int> &clipPath, offset_t splitTime,
                     bool broadcast = true);

    QString name() const override { return QStringLiteral("split-clip"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    QList<int> clipPath_;
    offset_t   splitTime_ = 0;
    // Edit groups: fan out to the corresponding clips of the track's group
    // members (one atomic composite). Fan-out children carry broadcast=false.
    bool       broadcast_ = true;
};

#endif // SSPLITCLIPACTION_H
