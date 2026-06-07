#ifndef SUNSPLITCLIPACTION_H
#define SUNSPLITCLIPACTION_H

#include "../saction.h"
#include "tw303aenv.h"
#include <QList>

// Inverse of SSplitClipAction: delete the second part and restore the first
// part's pre-split length. Its own inverse re-splits at the same point. Created
// live only (not registered/serialized for instantiation).
class SUnsplitClipAction : public SAction {
public:
    SUnsplitClipAction(const QList<int> &firstPath, const QList<int> &secondPath,
                       length_t restoreDuration, offset_t inObjOffset);

    QString name() const override { return QStringLiteral("unsplit-clip"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    QList<int> firstPath_;
    QList<int> secondPath_;
    length_t   restoreDuration_;
    offset_t   inObjOffset_;
};

#endif // SUNSPLITCLIPACTION_H
