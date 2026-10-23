#ifndef SREMOVESAMPLEACTION_H
#define SREMOVESAMPLEACTION_H

#include "app/actions/saction.h"
#include "app/model/slink.h"
#include "tw/core/twfraction.h"
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

    // --- the pin, which is the DEFAULT inverse (QBX-146 step a, QBX-202) ---
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
    //
    // IT PINS EVERY CONTENT IT IS NOT ASKED TO REBUILD (QBX-202). Step (a)
    // restricted the pin to a take column, which left every OTHER
    // un-rebuildable shape refused -- and the commonest of those is an
    // ordinary MIDI clip, whose content is an SMidiSequence: not an
    // SExternFile, not a path container, not a column. Deleting a recorded
    // MIDI clip therefore did nothing at all but log, which is QBX-202 as the
    // author met it. The pin needs to know nothing about what it holds, so
    // there was never a fidelity reason to narrow it; the one shape that must
    // stay refused is a LIVE recording, and that is OWNERSHIP, not fidelity.
    SObject *heldObject() const { return heldObject_; }
    void     releaseHeld();      // drop the pin, after the clip is re-attached
    // Release a pin left by a PREVIOUS apply. Called first on every apply:
    // this action is also add-sample's live inverse, so each redo of an
    // add-sample re-applies it against a NEW clip and would otherwise leak one
    // pin per redo.
    void     dropStalePin();

    /**
     * THE DELETED LINK'S OWN FIELDS, which the pin does NOT carry (QBX-202).
     *
     * The pin preserves the CONTENT by identity, but the SLink is destroyed
     * and the inverse builds a new one -- so everything stored on the
     * placement rather than on the object is born at its default instead:
     * `timebase_`, `startTicks_` and `eventChannelOverride_`. A fresh link
     * reads its timebase from SLink::defaultTimebaseFor(), so a MIDI clip
     * switched to the TIME timebase came back on beats, and a placement
     * channel remap came back as-authored.
     *
     * It is the same class of loss the pin exists to avoid, one level up, and
     * it was already there for a take column before this ticket widened the
     * reach. Captured on the forward's apply and read back by the inverse
     * through the same borrowed back-pointer the pin uses, so a redo that
     * re-captures is what the next undo restores.
     */
    struct PinnedLink {
        SLink::Timebase timebase = SLink::Timebase::Time;
        Fraction        startTicks{ 0 };
        int             eventChannelOverride = -1;
    };
    const PinnedLink &pinnedLink() const { return pinnedLink_; }

private:
    QList<int> trackPath_;
    int clipIndex_ = -1;
    QString filePath_;
    offset_t timePos_ = 0;

    SObject *heldObject_ = nullptr;   // the deleted clip's object, kept alive
    bool     holdsRef_   = false;     // ...by this pin
    PinnedLink pinnedLink_;           // ...and what the LINK itself carried
};

#endif // SREMOVESAMPLEACTION_H
