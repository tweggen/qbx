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
    uint16_t nIn_, nOut_;
};

#endif // SINSERTPLUGINACTION_H
