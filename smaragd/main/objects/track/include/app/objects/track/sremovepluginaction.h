#ifndef SREMOVEPLUGINACTION_H
#define SREMOVEPLUGINACTION_H

#include "app/actions/saction.h"

// Action: remove a plugin from a track's effect chain.
// Path: "/mixer/track[trackIdx]/plugins[slotIdx]"
// Inverse: SInsertPluginAction (re-inserts the removed plugin)
class SRemovePluginAction : public SAction {
public:
    // trackPath: path to track (e.g. "/mixer/0")
    // slotIndex: slot to remove
    SRemovePluginAction(const QString &trackPath, int slotIndex);

    QString name() const override { return QStringLiteral("remove-plugin"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    QString trackPath_;
    int slotIndex_;
    // Saved state for inverse re-insertion
    QString format_;
    QString uid_;
    QString pluginName_;
    QString vendor_;
    // The module file, carried so the inverse (re-insert) can actually load the
    // plugin again — see SInsertPluginAction::path_.
    QString path_;
    uint16_t nIn_, nOut_;
    // Base64 of the slot's opaque plugin state chunk, captured in apply() from
    // the LIVE plugin. proposal 08 M5: without it the inverse re-inserted the
    // plugin with DEFAULT parameters, so undoing a removal silently discarded
    // the user's patch — the model came back, the sound did not.
    QString state_;
};

#endif // SREMOVEPLUGINACTION_H
