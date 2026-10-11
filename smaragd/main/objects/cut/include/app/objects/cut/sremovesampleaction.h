#ifndef SREMOVESAMPLEACTION_H
#define SREMOVESAMPLEACTION_H

#include "app/actions/saction.h"
#include "tw/graph/tw303aenv.h"
#include <QList>
#include <QString>
#include <QDomElement>

class SObject;

// Action: remove a sample/clip from a track.
// Inverse reconstructs the sample from captured file path and position.
//
// The lane is addressed by an index-path from the root mixer ({2} = the 3rd
// top-level track, {0,1} = the 2nd child of the 1st), like every other clip
// verb. It used to be a single top-level index, which made a clip on a track
// nested inside a folder ("grouped") impossible to delete: the UI slot resolved
// -1 and returned without submitting anything, silently. The legacy
// `trackIndex` attribute is still ACCEPTED on read so existing .qxa scripts
// keep working.
class SRemoveSampleAction : public SAction {
public:
    SRemoveSampleAction() = default;
    SRemoveSampleAction(const QList<int> &trackPath, int clipIdx,
                        const QString &filePath, offset_t timePos);

    ~SRemoveSampleAction() override;

    // Holding a refcount pin makes this action non-copyable: a copy would
    // release the same reference twice. SRemoveTrackAction, the pattern this
    // follows, has the same exposure and no guard -- worth the same one-liner
    // there.
    SRemoveSampleAction( const SRemoveSampleAction & ) = delete;
    SRemoveSampleAction &operator=( const SRemoveSampleAction & ) = delete;

    QString name() const override { return QStringLiteral("remove-sample"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

    // --- the pin, for content nothing else can rebuild (QBX-146 step a) ----
    //
    // A clip whose content is an STakeStack cannot be reconstructed from
    // parameters: a column carries N takes with their own windows, fades,
    // names, pan and automation, an active index, and a comp map. Rebuilding
    // it from a captured list would drop whatever the list forgot -- the exact
    // failure SAddSampleAction once had, restoring "a full-length unedited
    // clip... silently, with no dialog to notice".
    //
    // So this action PINS the content instead, the way SRemoveTrackAction pins
    // a removed track: addRef() before the detach, the inverse re-links the
    // very same object and releases. Identity preserves every field for free,
    // including ones nobody has thought of yet.
    SObject *heldObject() const { return heldObject_; }
    void     releaseHeld();      // drop the pin, after the clip is re-attached
    // Release a pin left by a PREVIOUS apply. Called first on every apply:
    // this action is also add-sample's live inverse, so each redo of an
    // add-sample re-applies it against a NEW clip and would otherwise leak one
    // pin per redo.
    void     dropStalePin();

private:
    QList<int> trackPath_;
    int clipIndex_ = -1;
    QString filePath_;
    offset_t timePos_ = 0;

    SObject *heldObject_ = nullptr;   // the deleted clip's object, kept alive
    bool     holdsRef_   = false;     // ...by this pin
};

#endif // SREMOVESAMPLEACTION_H
