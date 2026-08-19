#ifndef SCLICKLANEACTION_H
#define SCLICKLANEACTION_H

#include "app/actions/saction.h"
#include "tw/core/twtypes.h"
#include <QString>

// click-lane — one real click (press+release, no move) into a track's LANE
// (the clip-drawing area, as opposed to its head/control column).
//
//   trackPath = "0"      index-path from the root mixer
//   time      = "0"      project frame to click at; turned into a pixel by
//                         the arranger's own zoom/scroll, exactly as a real
//                         click would be
//   modifiers = ""       "", "ctrl", "shift", or "ctrl+shift" — same spelling
//                         as select-track. Applied to BOTH the track
//                         selection (SStdMixerView::applyTrackSelectionClick,
//                         the same rule the head already uses) and, when the
//                         click lands on a clip, the clip's own selection.
//
// Runs the REAL mouse handlers (SMVActualView::mousePressEvent /
// mouseReleaseEvent) via SStdMixerView::clickLane — the drag-clip-edge twin
// for a click that may hit no clip at all. Lands on a clip if one covers
// `time` on that track's composite lane, empty lane space otherwise.
// Not undoable itself: track/clip selection is view state; anything the
// click submits as a model action (it never does for a zero-movement click,
// but the handlers are shared with the drag gestures) carries its own
// inverse.
class SClickLaneAction : public SAction {
public:
    QString name() const override { return QStringLiteral( "click-lane" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString  trackPath_ = QStringLiteral( "0" );
    offset_t time_ = 0;
    QString  modifiers_;
};

// split-selection — the split command ('s' / "Split object"), run exactly as
// a real keypress or menu click runs it: SStdMixerView::ctSplitSample().
// Splits every clip in the CURRENT SELECTION whose extent strictly contains
// the global locator (start < locator < end), or — when nothing is selected —
// the last-clicked clip. Takes no attributes: set up the selection (or the
// last click, via click-lane / drag-clip-edge) and the locator first.
class SSplitSelectionAction : public SAction {
public:
    QString name() const override { return QStringLiteral( "split-selection" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
};

#endif  // SCLICKLANEACTION_H
