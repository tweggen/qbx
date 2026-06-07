#ifndef SRESIZECLIPACTION_H
#define SRESIZECLIPACTION_H

#include "../saction.h"
#include "tw303aenv.h"
#include <QList>

// Action: set an SCut clip's placement — its link start time, the cut's start
// offset into the content, and the cut duration. The undoable form of dragging a
// clip's left/right edge. Inverse restores the previous placement (so it round-
// trips on undo/redo). The clip is addressed by track-path + link index.
class SResizeClipAction : public SAction {
public:
    SResizeClipAction() = default;
    SResizeClipAction( const QList<int> &clipPath,
                       offset_t startTime, offset_t startOffset, length_t duration );

    QString name() const override { return QStringLiteral("resize-clip"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> clipPath_;
    offset_t   startTime_   = 0;
    offset_t   startOffset_ = 0;
    length_t   duration_    = 0;
};

#endif // SRESIZECLIPACTION_H
