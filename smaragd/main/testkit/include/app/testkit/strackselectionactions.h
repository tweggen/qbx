#ifndef STRACKSELECTIONACTIONS_H
#define STRACKSELECTIONACTIONS_H

#include "app/actions/saction.h"
#include <QString>

// Multi-track selection test verbs.
//
// The feature under test is a GESTURE plus a broadcast, and neither is
// expressible as a sequence of model actions: a script that wrote the mixer's
// selection itself and then submitted one set-track-mute per track would prove
// only that the script can loop. So select-track drives the REAL head click
// (with its modifiers) and track-head-toggle presses the REAL M/S/R button;
// assert-track-selection then reads the resulting set off the model. Same
// shape as drag-clip-edge and group-track (testkit CONTRACT inv. 5) — the
// route runs through SMainWindow because testkit may not include app/timeline.

// select-track — one head click.
//
//   trackPath  = "0"      index-path from the root mixer
//   modifiers  = ""       "", "ctrl", "shift", or "ctrl+shift"
//
// Plain replaces the selection with this lane, ctrl toggles its membership,
// shift selects the lane range from the anchor (the last non-shift click).
// Not undoable — a selection is view state here, not project state.
class SSelectTrackAction : public SAction {
public:
    QString name() const override { return QStringLiteral( "select-track" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString trackPath_ = QStringLiteral( "0" );
    QString modifiers_;
};

// track-head-toggle — press a track head's toggle button.
//
//   trackPath = "0"       index-path from the root mixer (must have a HEAD,
//                         i.e. its lane is visible)
//   control   = "mute"    mute | solo | arm | takes | group
//   on        = "1"       the state to drive the button TO (no-op if already)
//
// This is the only route to the broadcast: pressing M on a head that is part
// of a multi-selection has to reach every selected lane, as ONE undo step.
class STrackHeadToggleAction : public SAction {
public:
    QString name() const override { return QStringLiteral( "track-head-toggle" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString trackPath_ = QStringLiteral( "0" );
    QString control_   = QStringLiteral( "mute" );
    bool    on_        = true;
};

// drag-track — grip-drag a track head and drop it on a lane ROW.
//
//   trackPath = "1"       the head that is grabbed (index-path)
//   targetRow = "0"       the lane row to drop on, in the flattened tree
//   mode      = "onto"    onto = nest into that row's track
//                         before = insert at that row's top boundary
//                         (targetRow == the row count means "below the last")
//
// Grabbing a head that is part of the selection drags the WHOLE selection;
// grabbing any other head selects it first and drags it alone. Rows, not
// pixels, so the script does not encode the current zoom.
class SDragTrackAction : public SAction {
public:
    QString name() const override { return QStringLiteral( "drag-track" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString trackPath_ = QStringLiteral( "0" );
    int     targetRow_ = 0;
    bool    nestOnto_  = true;
};

// fader-key — send Home/End straight to a track head's LIVE fader (item j).
//
//   trackPath = "0"       index-path from the root mixer (must have a HEAD)
//   key       = "Home"    "Home" or "End"
//
// The point is that the fader must never react to Home/End itself — they
// must reach the transport (SMainWindow::gotoRangeStart/gotoRangeEnd) exactly
// as if the fader had never been focused. See
// SSMVMixerControl::tkSendFaderKey: it delivers a real QKeyEvent through
// QApplication::sendEvent, the same dispatch a real keystroke uses, so this
// exercises SSMVMixerControl::eventFilter's interception rather than a
// re-spelling of it. Not undoable — like select-track, it drives a UI gesture,
// not a model edit (the transport move it triggers is asserted separately,
// e.g. by wait-playhead/assert-source-position).
class SFaderKeyAction : public SAction {
public:
    QString name() const override { return QStringLiteral( "fader-key" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString trackPath_ = QStringLiteral( "0" );
    QString key_       = QStringLiteral( "Home" );
};

// head-fader — drive a lane's HEAD fader to a dB value (proposal 45 AC4.4).
//
//   trackPath = "0"   index-path from the root mixer; `$master` reaches the
//                     master lane, which is the case this verb exists for
//   db        = "0"   the dB to move the fader to
//
// It drives the SLIDER, so the real chain runs: valueChanged -> sliderToDB ->
// applyVolume_ -> strackpath::pathOf() -> set-track-volume. That chain is what
// is under test -- a head whose slider moves and whose action never fires is
// D9's write-side defect, and only a HEARD level separates the two.
//
// REFUSED when the lane has no head, which is exactly what a HIDDEN lane means,
// and when the fader already holds the requested tick (setValue would emit
// nothing, so the case would assert nothing while reporting success).
//
// NOT undoable itself: the handler submits the real set-track-volume, and that
// is the undo step -- the automation-UI-gesture rule.
//
// THE CURVE DOES NOT ROUND-TRIP (sDbToFader(0.0) is tick -191, whose dB is
// +0.0625), so assert a BAND around what you asked for, never an exact value.
class SHeadFaderAction : public SAction {
public:
    QString name() const override { return QStringLiteral( "head-fader" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString trackPath_ = QStringLiteral( "0" );
    double  db_        = 0.0;
};

// assert-track-volume — a track's fader value, in dB.
//
//   trackPath = "0"       index-path from the root mixer
//   volumeDb  = "0"       expected value (SObject::getVolume(), dB)
//   tolerance = "0.001"
//
// Exists to prove a gesture did NOT move a fader — fader-key's whole point
// (item j) is that Home/End reach the transport and leave the fader alone,
// which no existing assert verb could state.
class SAssertTrackVolumeAction : public SAction {
public:
    QString name() const override { return QStringLiteral( "assert-track-volume" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString trackPath_ = QStringLiteral( "0" );
    double  volumeDb_  = 0.0;
    double  tolerance_ = 0.001;
};

// assert-track-selection — the mixer's selection, as index-paths.
//
//   paths   = "0;1"       SEMICOLON-separated (a path itself is
//                         comma-separated), compared as a SET
//   primary = "1"         optional: the path that must be the primary
//
// An empty paths="" asserts nothing is selected.
class SAssertTrackSelectionAction : public SAction {
public:
    QString name() const override { return QStringLiteral( "assert-track-selection" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString paths_;
    QString primary_;
    bool    hasPrimary_ = false;
};

// assert-track-name — a track's display name (SObject::getSName()).
//
//   trackPath = "0"       index-path from the root mixer
//   name      = ""        exact match; checked only when non-empty
//   prefix    = ""        name.startsWith(prefix); checked only when non-empty
//   suffix    = ""        name.endsWith(suffix); checked only when non-empty
//   differsFrom = ""      another track's index-path; asserts the two names
//                         are NOT equal (checked only when non-empty)
//
// At least one of name/prefix/suffix/differsFrom must be given. Exists for
// add-track's generated default name: the disambiguation strategy (vary the
// END of the name before the front — a narrow track-control column shows the
// front and drops the tail) has no other observable surface, since
// set-track-name overwrites whatever add-track picked.
class SAssertTrackNameAction : public SAction {
public:
    QString name() const override { return QStringLiteral( "assert-track-name" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString trackPath_;
    QString name_;
    QString prefix_;
    QString suffix_;
    QString differsFrom_;
};

#endif  // STRACKSELECTIONACTIONS_H
