#ifndef SREMOVECLIPACTION_H
#define SREMOVECLIPACTION_H

#include "app/actions/saction.h"
#include "tw/graph/tw303aenv.h"
#include <QList>

// Inverse of SDuplicateClipAction and of SLinkClipAction: delete the clip at
// clipPath. Its own inverse re-creates it from the original (carried in
// source/dest/startTime), so redo re-creates it. Created live only — not
// registered/serialized.
//
// `share` SAYS WHICH FORWARD VERB TO REBUILD (QBX-217), and it has to be
// carried rather than inferred: a duplicate and a shared link are the same
// shape in the model afterwards — one link on a lane — so nothing about the
// clip being removed says which gesture made it.
//
// IT IS CORRECTNESS IN DEPTH AND IS CURRENTLY UNOBSERVABLE, which is said here
// because the obvious reading of the paragraph above — "otherwise redo hands
// back an independent copy" — is WRONG, and a sabotage is what established
// that. REDO DOES NOT USE THIS INVERSE. `SActionUndoCommand::redo()`
// re-applies the stored FORWARD action, and the inverse this class returns is
// handed to `SActionHistory::submit( …, skipHistory=true )`, which applies it
// and then `delete result.inverse` — measured in sactionhistory.cpp. Since
// this class is created live only, as somebody else's inverse, its own inverse
// is discarded on the only path that ever builds it.
//
// So redo re-shares because the forward action is a `link-clip`, not because
// of this bit. The bit stays because the day `remove-clip` becomes a verb a
// script or a macro submits FORWARD, its inverse starts being used and
// rebuilding the wrong gesture would be silent. NOT GATED, for the reason
// above: no case can see it, and one that claimed to would be gating
// fiction.
class SRemoveClipAction : public SAction {
public:
    SRemoveClipAction( const QList<int> &clipPath,
                       const QList<int> &sourceClipPath,
                       const QList<int> &destTrackPath,
                       offset_t startTime,
                       bool share = false );

    QString name() const override { return QStringLiteral("remove-clip"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> clipPath_;         // the duplicated clip to remove
    QList<int> sourceClipPath_;   // original, for re-duplicating on redo
    QList<int> destTrackPath_;
    offset_t   startTime_ = 0;
    bool       share_ = false;   // see the class comment
};

#endif // SREMOVECLIPACTION_H
