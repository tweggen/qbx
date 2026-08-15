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

#endif  // STRACKSELECTIONACTIONS_H
