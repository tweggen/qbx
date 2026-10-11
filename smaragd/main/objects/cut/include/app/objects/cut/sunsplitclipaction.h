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
};

#endif // SUNSPLITCLIPACTION_H
