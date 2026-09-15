#ifndef SSETCLIPPANACTION_H
#define SSETCLIPPANACTION_H

#include "app/actions/saction.h"
#include <QList>

// Action: set a clip's pan position, -1.0 (full left) .. +1.0 (full right),
// 0 = center (the default; see SObject::pan_ / getPan() / setPan(), which
// already clamps to this range). The value is ABSOLUTE (undo restores the
// exact previous number — mirrors set-clip-volume / set-pitch). Take stacks:
// a PER-TAKE property, same rule as volume/pitch/formant.
//
// AUDIBLE since proposal 49 M1. The value reaches twTrackMix's clip loop
// through STrack::refreshClipGainCurves(), and SCut::onPanChanged invalidates
// the render path. The law is tw/mix/twpanlaw.h, applied per OUTPUT channel.
//
// Pan is defined at a project width of 2 only (proposal 49 D1): at any other
// width a NON-ZERO value is REFUSED with a warning, and 0 is always accepted.
// An EVENT clip (SMidiCut) is refused because it is not an SCut (D2): an
// instrument's audio is under TRACK pan.
class SSetClipPanAction : public SAction {
public:
    SSetClipPanAction() = default;
    SSetClipPanAction( const QList<int> &clipPath, double pan,
                       int take = -1, bool broadcast = true );

    QString name() const override
        { return QStringLiteral("set-clip-pan"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> clipPath_;
    double     pan_       = 0.0;
    int        take_      = -1;
    bool       broadcast_ = true;
};

#endif // SSETCLIPPANACTION_H
