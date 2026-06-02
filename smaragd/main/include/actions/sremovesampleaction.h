#ifndef SREMOVESAMPLEACTION_H
#define SREMOVESAMPLEACTION_H

#include "../saction.h"
#include "tw303aenv.h"
#include <QString>
#include <QDomElement>

// Action: remove a sample/clip from a track.
// Inverse reconstructs the sample from captured file path and position.
class SRemoveSampleAction : public SAction {
public:
    SRemoveSampleAction() = default;
    SRemoveSampleAction(int trackIdx, int clipIdx, const QString &filePath, offset_t timePos);

    QString name() const override { return QStringLiteral("remove-sample"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    int trackIndex_ = -1;
    int clipIndex_ = -1;
    QString filePath_;
    offset_t timePos_ = 0;
};

#endif // SREMOVESAMPLEACTION_H
