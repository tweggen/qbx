#ifndef STOGGLEPLAYBACKACTION_H
#define STOGGLEPLAYBACKACTION_H

#include "../saction.h"

// Action: toggle playback (play/pause).
// Not undoable (transport control).
class STogglePlaybackAction : public SAction {
public:
    explicit STogglePlaybackAction(bool play);

    QString name() const override { return QStringLiteral("toggle-playback"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    bool play_;
};

#endif // STOGGLEPLAYBACKACTION_H
