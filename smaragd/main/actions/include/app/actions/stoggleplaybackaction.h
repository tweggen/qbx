#ifndef STOGGLEPLAYBACKACTION_H
#define STOGGLEPLAYBACKACTION_H

#include "app/actions/saction.h"

// Action: toggle playback (play/pause).
// Not undoable: playback state is transient (not persisted in project).
// Undo/redo only tracks persistent changes to project content.
class STogglePlaybackAction : public SAction {
public:
    explicit STogglePlaybackAction(bool play);

    QString name() const override { return QStringLiteral("toggle-playback"); }
    // Despite the name this verb is ABSOLUTE, not a toggle: `play` defaults to
    // "0", so a bare <toggle-playback/> STOPS. Declaring the attribute at least
    // makes a case that spells it wrong say so.
    QStringList knownAttributes() const override { return {QStringLiteral("play")}; }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    bool play_;
};

#endif // STOGGLEPLAYBACKACTION_H
