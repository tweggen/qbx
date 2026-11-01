#ifndef SINSERTPLUGINACTION_H
#define SINSERTPLUGINACTION_H

#include "app/actions/saction.h"
#include <QList>
#include <QPointer>
#include <QtGlobal>
#include <vector>

namespace audio {
struct twPluginDescriptor;
}
class SObject;

// Action: insert a plugin at a given slot index on a track.
// Path: "/mixer/track[trackIdx]/plugins"
// Inverse: SRemovePluginAction (removes the inserted plugin)
class SInsertPluginAction : public SAction {
public:
    // trackPath: comma-separated index path from the root mixer ("0" = first
    //            track); see app/model/sobjectpath.h
    // slotIndex: -1 = append; otherwise insert at that index
    // descriptor: plugin to insert (format, uid, name, I/O layout)
    // stateBase64: OPTIONAL opaque plugin state chunk, base64 of the same bytes
    //            the project file's <state> child carries. Empty = plugin
    //            defaults. This is what makes the inverse of remove-plugin
    //            faithful (proposal 08 M5): before it existed, undoing a removal
    //            brought the slot back with DEFAULT parameters, silently
    //            discarding the user's patch.
    SInsertPluginAction(
        const QString &trackPath,
        int slotIndex,
        const audio::twPluginDescriptor &descriptor,
        const QString &stateBase64 = QString(),
        quint64 slotId = 0
    );
    ~SInsertPluginAction() override;

    QString name() const override { return QStringLiteral("insert-plugin"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

    // THE SLOT'S MODEL CHILDREN (QBX-254; objects/track inv. on model
    // children): set only on the inverse of a remove-plugin whose slot held
    // SObject children through its SLink loop -- NassauDigital's program is
    // one. The undo re-creates a NEW slot object, and a model child is in
    // neither the descriptor nor the state blob, so without this the restored
    // slot came back without it. The children are carried by OBJECT and PINNED
    // (addRef) for this action's lifetime: qbx's history keeps ONE inverse for
    // every undo and redoes by re-running the forward remove-plugin, so the
    // same objects must survive every redo's removal. Re-adopted, in order,
    // BEFORE the link is parented, so the child-adopted hook binds them before
    // the instances exist. Live state, NOT serialized -- like the slot id's
    // object identity, it has no XML spelling.
    void setModelChildren(const QList<SObject *> &children);

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
    // Tri-state, because a hand-written .qxa may legitimately omit it: 1/0 are
    // an explicit claim, -1 means "ask the registry" (which is how the built-in
    // `tw.native.303` works with no attribute at all). Written only when TRUE,
    // so every pre-P3b script re-serializes byte-identically.
    int isInstrument_ = -1;
    // Base64 of the opaque plugin state chunk; empty when the slot should come
    // up with plugin defaults. Written as the optional `state` attribute.
    QString state_;
    // The slot's stable id (proposal 55 D8). Non-zero only on the inverse of a
    // remove-plugin whose slot a return track named: the undo must bring the
    // slot back under the SAME id, or every return of it silently unroutes
    // (T5). Written only when non-zero.
    quint64 slotId_ = 0;
    // See setModelChildren(). Each non-null entry holds one reference.
    QList<QPointer<SObject>> modelChildren_;
};

#endif // SINSERTPLUGINACTION_H
