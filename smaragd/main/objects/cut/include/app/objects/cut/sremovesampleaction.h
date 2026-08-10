#ifndef SREMOVESAMPLEACTION_H
#define SREMOVESAMPLEACTION_H

#include "app/actions/saction.h"
#include "tw/graph/tw303aenv.h"
#include <QList>
#include <QString>
#include <QDomElement>

// Action: remove a sample/clip from a track.
// Inverse reconstructs the sample from captured file path and position.
//
// The lane is addressed by an index-path from the root mixer ({2} = the 3rd
// top-level track, {0,1} = the 2nd child of the 1st), like every other clip
// verb. It used to be a single top-level index, which made a clip on a track
// nested inside a folder ("grouped") impossible to delete: the UI slot resolved
// -1 and returned without submitting anything, silently. The legacy
// `trackIndex` attribute is still ACCEPTED on read so existing .qxa scripts
// keep working.
class SRemoveSampleAction : public SAction {
public:
    SRemoveSampleAction() = default;
    SRemoveSampleAction(const QList<int> &trackPath, int clipIdx,
                        const QString &filePath, offset_t timePos);

    QString name() const override { return QStringLiteral("remove-sample"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    QList<int> trackPath_;
    int clipIndex_ = -1;
    QString filePath_;
    offset_t timePos_ = 0;
};

#endif // SREMOVESAMPLEACTION_H
