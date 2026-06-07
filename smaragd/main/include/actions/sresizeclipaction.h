#ifndef SRESIZECLIPACTION_H
#define SRESIZECLIPACTION_H

#include "../saction.h"
#include "tw303aenv.h"
#include <QList>

// Action: set an SCut clip's whole window — its link start time, the cut's start
// offset into the content, the cut duration, the loop-segment length, and the
// grain stretch factor. The undoable form of every clip-edge gesture (trim,
// extend, slip, loop, time-stretch). Inverse restores the previous window (so it
// round-trips on undo/redo). The clip is addressed by track-path + link index.
class SResizeClipAction : public SAction {
public:
    SResizeClipAction() = default;
    SResizeClipAction( const QList<int> &clipPath,
                       offset_t startTime, offset_t startOffset, length_t duration,
                       length_t loopLength = 0, double stretch = 1.0 );

    QString name() const override { return QStringLiteral("resize-clip"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> clipPath_;
    offset_t   startTime_   = 0;
    offset_t   startOffset_ = 0;
    length_t   duration_    = 0;
    length_t   loopLength_  = 0;
    double     stretch_     = 1.0;
};

#endif // SRESIZECLIPACTION_H
