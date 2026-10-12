#ifndef SUNSPLITCLIPACTION_H
#define SUNSPLITCLIPACTION_H

#include "app/actions/saction.h"
#include "tw/events/twcompmap.h"
#include "tw/graph/tw303aenv.h"
#include <QList>

// Inverse of SSplitClipAction: delete the second part and restore the first
// part's pre-split length. Its own inverse re-splits at the same point. Created
// live only (not registered/serialized for instantiation).
class SUnsplitClipAction : public SAction {
public:
    SUnsplitClipAction(const QList<int> &firstPath, const QList<int> &secondPath,
                       length_t restoreDuration, offset_t inObjOffset);

    QString name() const override { return QStringLiteral("unsplit-clip"); }
    /**
     * Restore THIS comp map onto the re-joined column (plan 53 B3).
     *
     * An inverse-only payload, as SRemoveTakeAction::setRestoreCompMap is
     * (QBX-159): a scripted `unsplit-clip` never carries it, and it is not
     * serialized. The split halves cannot reconstruct the original between
     * them -- the head's segments beyond the split were dropped and the tail's
     * opening segment was synthesised -- so only the splitting action can hand
     * it back.
     */
    void setRestoreCompMap( const twCompMap &map )
    { restoreMap_ = map; hasRestoreMap_ = true; }

    /**
     * ONE PRE-SPLIT TAKE, for the MIXED inverse of a take-column split
     * (plan 53 §4.1).
     *
     * `origIndex` is the take's index in the column BEFORE the split, which is
     * the order the re-joined column must come out in. `tStart`/`tLen` are its
     * pre-split extent. `fromTail` says where the OBJECT is now: a take wholly
     * in the tail was MOVED there and comes back by identity, while every
     * other take stayed in the head (a crossing one trimmed, a head-only one
     * untouched) and is restored by extent alone. `halfIndex` is its index in
     * whichever half it is in now.
     *
     * The tail's CLONES of crossing takes are not listed: they die with the
     * tail link, and the head's originals are re-grown instead -- which is
     * what `SUnsplitClipAction` always did, for exactly one reason that has
     * not changed (a clone is not the object the user's fades are on).
     */
    struct TakeRestore {
        int      origIndex = 0;
        offset_t tStart    = 0;
        length_t tLen      = 0;
        bool     fromTail  = false;
        int      halfIndex = 0;
    };

    /**
     * The per-take record, in ASCENDING origIndex. Inverse-only and
     * unserialized, like setRestoreCompMap: which half a take came from is
     * precisely the information the split consumed, so only the splitting
     * action can hand it back.
     */
    void setTakeRestore( const QList<TakeRestore> &recs, int activeTake )
    { takes_ = recs; restoreActive_ = activeTake; hasTakes_ = true; }

    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    QList<int> firstPath_;
    QList<int> secondPath_;
    length_t   restoreDuration_;
    offset_t   inObjOffset_;
    twCompMap  restoreMap_;
    bool       hasRestoreMap_ = false;
    QList<TakeRestore> takes_;
    bool       hasTakes_ = false;
    int        restoreActive_ = -2;
};

#endif // SUNSPLITCLIPACTION_H
