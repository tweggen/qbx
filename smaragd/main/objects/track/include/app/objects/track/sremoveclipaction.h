#ifndef SREMOVECLIPACTION_H
#define SREMOVECLIPACTION_H

#include "app/actions/saction.h"
#include "tw/graph/tw303aenv.h"
#include <QList>

// Inverse of SDuplicateClipAction: delete the duplicated clip at clipPath. Its
// own inverse re-duplicates from the original (carried in source/dest/startTime),
// so redo re-creates the copy. Created live only — not registered/serialized.
class SRemoveClipAction : public SAction {
public:
    SRemoveClipAction( const QList<int> &clipPath,
                       const QList<int> &sourceClipPath,
                       const QList<int> &destTrackPath,
                       offset_t startTime );

    QString name() const override { return QStringLiteral("remove-clip"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> clipPath_;         // the duplicated clip to remove
    QList<int> sourceClipPath_;   // original, for re-duplicating on redo
    QList<int> destTrackPath_;
    offset_t   startTime_ = 0;
};

#endif // SREMOVECLIPACTION_H
