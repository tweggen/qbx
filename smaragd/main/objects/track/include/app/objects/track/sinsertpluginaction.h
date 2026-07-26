#ifndef SINSERTPLUGINACTION_H
#define SINSERTPLUGINACTION_H

#include "app/actions/saction.h"
#include <vector>

namespace audio {
struct twPluginDescriptor;
}

// Action: insert a plugin at a given slot index on a track.
// Path: "/mixer/track[trackIdx]/plugins"
// Inverse: SRemovePluginAction (removes the inserted plugin)
class SInsertPluginAction : public SAction {
public:
    // trackPath: path to track (e.g. "/mixer/0")
    // slotIndex: -1 = append; otherwise insert at that index
    // descriptor: plugin to insert (format, uid, name, I/O layout)
    SInsertPluginAction(
        const QString &trackPath,
        int slotIndex,
        const audio::twPluginDescriptor &descriptor
    );

    QString name() const override { return QStringLiteral("insert-plugin"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    QString trackPath_;
    int slotIndex_;
    QString format_;
    QString uid_;
    QString pluginName_;
    QString vendor_;
    // The module file. Without it a format="clap" descriptor cannot be
    // instantiated at all — the registry loads the DSO by path — so an action
    // script (and undo of a remove) had no way to name a real plugin. A
    // RELATIVE path resolves against the project's sample base dir (the .qxa's
    // own directory) and then against the application directory, which is what
    // lets a test case reference the in-repo twtestclap fixture without knowing
    // the build layout.
    QString path_;
    uint16_t nIn_, nOut_;
};

#endif // SINSERTPLUGINACTION_H
