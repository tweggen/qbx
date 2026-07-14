#ifndef SRESIZECLIPACTION_H
#define SRESIZECLIPACTION_H

#include "app/actions/saction.h"
#include "tw/core/twfraction.h"
#include "tw/graph/tw303aenv.h"
#include <QList>

// Action: set an SCut clip's whole window — its link start time, the cut's start
// offset into the content, the cut duration, the loop-segment length, and the
// grain stretch factor. The undoable form of every clip-edge gesture (trim,
// extend, slip, loop, time-stretch). Inverse restores the previous window (so it
// round-trips on undo/redo). The clip is addressed by track-path + link index.
//
// Take stacks (proposal 17): duration/loopLength/stretch write through to
// EVERY take (length ops affect all lanes); startOffset — the slip — applies
// to ONE take only, selected by `take` (-1 = the active take).
class SResizeClipAction : public SAction {
public:
    SResizeClipAction() = default;
    SResizeClipAction( const QList<int> &clipPath,
                       offset_t startTime, const Fraction &srcStart, length_t duration,
                       length_t loopLength = 0, const Fraction &stretch = Fraction(1),
                       int take = -1, bool broadcast = true );

    QString name() const override { return QStringLiteral("resize-clip"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> clipPath_;
    offset_t   startTime_   = 0;
    Fraction   srcStart_    = Fraction(0);  // exact source-domain slip anchor
    length_t   duration_    = 0;
    length_t   loopLength_  = 0;
    Fraction   stretch_     = Fraction(1);
    int        take_        = -1;   // stacks only: which take the slip targets
    // Edit groups: fan out to the members' corresponding clips. The slip is
    // synced by EXPLICIT take index (decision 3), so a stack anchor resolves
    // its take before fanning out.
    bool       broadcast_   = true;
};

#endif // SRESIZECLIPACTION_H
