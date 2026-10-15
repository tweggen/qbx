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

// double-click-lane — click-lane's DOUBLE-click twin, and double-click-clip's
// twin for a double-click that may hit no clip at all.
//
//   trackPath = "0"      index-path from the root mixer
//   time      = "0"      project frame to double-click at
//   modifiers = ""       same spelling as click-lane
//
// Runs the REAL mouse handlers (SMVActualView::mouseDoubleClickEvent) via
// SStdMixerView::doubleClickLane — the only route to double-clicking a bare
// FOLDER LANE (no clip at `time`), which double-click-clip cannot reach
// because it requires an existing clip to address. Lands on a clip if one
// covers `time` (same as double-click-clip), empty lane space otherwise.
// Not undoable itself; whatever the real handler does (open a tab, show take
// lanes, toggle a fold) carries its own undo story where it has one.
class SDoubleClickLaneAction : public SAction {
public:
    QString name() const override { return QStringLiteral( "double-click-lane" ); }
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

// cleanup-takes-gesture (plan 53 §6.3, QBX-195) — the CLEAN-UP SWEEP AS A
// USER REACHES IT, which is a different thing from `cleanup-takes` and is why
// this verb exists at all.
//
// `cleanup_takes.qxa` already drives the VERB, so a second case submitting the
// same verb would gate nothing new. What P6 left unbuilt was everything
// between a menu click and that verb: resolving the scope, translating the
// clip SELECTION into `clips=` (which the verb cannot do -- `objects/cut` may
// not see a selection), asking the confirmation, and submitting the result as
// one undo step. This drives exactly that, through
// SMainWindow::cleanupTakesGesture -> SStdMixerView::cleanupTakesGesture, the
// same two calls the Edit menu and the lane context menu make.
//
//   scope             "selection" (default) — the Edit menu's and the dock
//                     button's scope: the CLIP SELECTION, or the whole project
//                     when nothing is selected. Set up the selection first
//                     (set-selection / click-lane).
//                     "tracks" — the lane context menu's scope: every take
//                     column on the last-clicked track and on the other
//                     selected tracks. Set up the click first (click-lane).
//   answer            "yes" (default) — supply the confirmation's two answers
//                     from the attributes below instead of opening it. This is
//                     the ONE seam a scripted run uses, and it has to exist:
//                     a modal in a headless run is a hang, so the interactive
//                     path can only ever DECLINE there.
//                     "ask" — drive the interactive path instead, which under
//                     --test-case declines and therefore removes nothing. That
//                     is the gate on "nothing happens without a confirmation".
//   includeOriginals  "0" — the dialog's first checkbox, as answered.
//   files             "0" — its second. **No case sets this to 1**: it moves
//                     files to the OS trash, which a -j4 suite may not do. The
//                     verb carries it so the attribute is reachable at all and
//                     so a hand-run can exercise it.
//   expectRemoved     "-1" (unset = not checked) — how many takes the gesture
//                     removed. ZERO is a legitimate answer (nothing to do, or
//                     declined), so this is EXACT rather than a floor: a gate
//                     that only said "at least one" could not tell the cancel
//                     branch from the sweep.
//
// Not undoable itself: the gesture submits its own `cleanup-takes`, and
// undoing THAT is what reverses the sweep.
class SCleanupTakesGestureAction : public SAction {
public:
    QString name() const override
    { return QStringLiteral( "cleanup-takes-gesture" ); }
    QStringList knownAttributes() const override
    {
        return { QStringLiteral( "scope" ), QStringLiteral( "answer" ),
                 QStringLiteral( "includeOriginals" ),
                 QStringLiteral( "files" ),
                 QStringLiteral( "expectRemoved" ) };
    }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString scope_            = QStringLiteral( "selection" );
    QString answer_           = QStringLiteral( "yes" );
    bool    includeOriginals_ = false;
    bool    files_            = false;
    int     expectRemoved_    = -1;
};

// wheel-scroll (AC-g1) — one REAL QWheelEvent to the arranger canvas, exactly
// as a physical notch would be delivered, so the follow-playhead HOLD
// (SMVActualView::armFollowHold) is exercised through its actual call site
// rather than by moving the view directly (set-lane-view's scrollX=, which
// bypasses every arm point on purpose).
//
//   deltaY    = "600"    angleDelta units — one QWheelEvent's worth of a
//                         single notch; positive = wheel "up".
//   modifiers = "shift"  which SOpt wheel-action slot fires: the arranger's
//                         default maps plain wheel to vertical (track)
//                         scroll and Shift+wheel to horizontal PAN, so
//                         "shift" is what a case wanting the follow-hold
//                         gate needs (see main/servicesui's SOpt::WheelShift).
//
// Not undoable: a wheel scroll is view state, never a model edit.
class SWheelScrollAction : public SAction {
public:
    QString name() const override { return QStringLiteral( "wheel-scroll" ); }
    QStringList knownAttributes() const override
    { return { QStringLiteral( "deltaY" ), QStringLiteral( "modifiers" ) }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    int     deltaY_ = 600;
    QString modifiers_;
};

// select-all (AC-a3) — Ctrl-A, driven through SMainWindow::
// sendSelectAllShortcut(): a ShortcutOverride offered to the current focus
// widget first, then either that widget's own keyPressEvent or (nothing
// claimed it) the window's "Select All" QAction — the same fork a real
// keystroke goes through. No attributes: what it does depends on WHICH
// WIDGET has focus (set that up first, e.g. with activate-tab or
// show-event-editor) exactly as a real Ctrl-A would.
//
// Not undoable itself: the arranger's answer is ONE SSetSelectionAction,
// which carries its own inverse; the piano roll's answer is view state.
class SSelectAllAction : public SAction {
public:
    QString name() const override { return QStringLiteral( "select-all" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
};

// deselect-all — the Ctrl(Cmd)+Shift+A twin of select-all, driven through
// SMainWindow::sendDeselectAllShortcut(). Same fork, same caveat: headlessly
// it can only ever exercise the arranger's default branch (clearing the
// active tab's clip selection as one undoable SClearSelectionAction), never
// the piano roll's own ShortcutOverride handling — QApplication::
// focusWidget() is always null in a --test-case run.
class SDeselectAllAction : public SAction {
public:
    QString name() const override { return QStringLiteral( "deselect-all" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
};

#endif  // SCLICKLANEACTION_H
