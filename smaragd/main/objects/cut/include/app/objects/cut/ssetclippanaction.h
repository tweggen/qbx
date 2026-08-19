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
// DELIBERATELY NOT WIRED INTO THE AUDIO PATH. Nothing downstream reads a
// clip's pan (the wide sink removed the reason it could not be heard, not the
// work — see CLAUDE.md's automation section on `self:Pan`, still absent for
// the same reason). This action only ever touches the model: unlike
// set-clip-volume it never calls invalidateRenderPathRange, because there is
// nothing in the render chain for a pan edit to invalidate.
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
