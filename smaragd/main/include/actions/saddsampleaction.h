#ifndef SADDSAMPLEACTION_H
#define SADDSAMPLEACTION_H

#include "../saction.h"
#include "tw303aenv.h"

class QString;

// Action: add a sample to a track at a given time position.
// Inverse: removes the sample (SRemoveSampleAction).
class SAddSampleAction : public SAction {
public:
    SAddSampleAction() = default;
    SAddSampleAction(int trackIdx, const QString &filePath, offset_t timePos);

    QString name() const override { return QStringLiteral("add-sample"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    int trackIndex_;
    QString filePath_;
    offset_t timePos_;
};

#endif // SADDSAMPLEACTION_H
