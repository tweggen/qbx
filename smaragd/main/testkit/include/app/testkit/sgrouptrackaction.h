#ifndef SGROUPTRACKACTION_H
#define SGROUPTRACKACTION_H

#include "app/actions/saction.h"
#include <QString>

// group-track — run the arranger's REAL Group / Ungroup context-menu slots.
//
//   trackPath = "0"      index-path from the root mixer of the track to act on
//   ungroup   = "0"      "1" = Ungroup (dissolve this folder), else Group
//
// These are gestures, not actions: SStdMixerView::ctGroupTrack() /
// ctUngroupTrack() compose a MACRO of add-track / reparent-track / remove-track
// and the index arithmetic in that composition is the whole risk — especially
// for a NESTED lane, where a new folder cannot be born in place (add-track only
// appends at the mixer's top level) and cannot be slid there afterwards
// (reparent-track refuses a same-container move). Re-spelling the macro in a
// .qxa script would test the script, not the gesture, so this verb drives the
// slots the menu drives. Same rationale as drag-clip-edge for clip gestures.
//
// Not undoable itself: the gesture pushes its own macro onto the undo stack, so
// a following <undo count="1"/> reverses the whole grouping.
class SGroupTrackAction : public SAction {
public:
    QString name() const override { return QStringLiteral( "group-track" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString trackPath_ = QStringLiteral( "0" );
    bool    ungroup_   = false;
};

#endif  // SGROUPTRACKACTION_H
