#ifndef SDUPLICATEASSETHEREACTION_H
#define SDUPLICATEASSETHEREACTION_H

#include "app/actions/saction.h"
#include "tw/graph/tw303aenv.h"
#include <QList>
#include <QString>

// Action: "Duplicate asset here" (proposal 41 D2/D9, M2 AC2.4) -- mint a NEW
// asset whose fragment is a DEEP COPY of an existing lane-fragment asset's,
// and repoint exactly ONE placement to it. The original asset and every other
// placement of it are left untouched and still sharing (AC2.4b): sharing is
// the invariant this proposal never breaks, and a variation is therefore a
// NEW asset, never an un-shared placement (D2 -- do not read this as
// "make-unique").
//
//   <duplicate-asset-here clip="0,3" name="Riff copy"/>
//
// `clip` addresses the EXISTING placement to repoint (a fragment-asset clip,
// the same qualified spelling unpack-clips uses); `name` is optional, empty
// generates "<asset> copy", then "<asset> copy 2", ....
//
// The deep copy reuses `makeDuplicateClip()` (sduplicateclipaction.h) per
// child, exactly the mechanism `duplicate-clip` already uses for "clone a
// clip": each child gets its OWN window object (a new SCut/SClipWindow) while
// its underlying CONTENT (the wave file, the sequence) stays shared -- which
// is right, because every per-clip edit this codebase has (gain, pitch, pan,
// formant, split, warp) mutates the WINDOW layer, never the raw content. A
// shallow structural copy (new SLinks over the SAME child cuts) would leave
// the original and the copy editing the same window objects, silently
// un-sharing nothing while APPEARING to.
//
// Composed of three primitives, one undo step (SCompositeAction, following
// place-recording's precedent): mint the new asset (a small live-only helper
// pair, undoable but not independently scriptable -- symmetrical with
// SCompositeAction itself), SRemoveAssetPlacementAction on the old placement,
// SPlaceAssetAction for the new one at the SAME track/time. Reusing
// SPlaceAssetAction verbatim is what gives AC2.6 for free: its existing
// `sarrangements::reaches` cycle guard applies to a fragment asset exactly as
// it already does to a container asset, with no second guard written here.
class SDuplicateAssetHereAction : public SAction {
public:
    SDuplicateAssetHereAction() = default;
    SDuplicateAssetHereAction( const QList<int> &clipPath,
                               const QString &newAssetName = QString() );

    QString name() const override
    { return QStringLiteral( "duplicate-asset-here" ); }
    QStringList knownAttributes() const override
    { return { QStringLiteral( "clip" ), QStringLiteral( "name" ) }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> clipPath_;
    QString    newAssetName_;   // empty => generate "<asset> copy[ N]"
};

#endif // SDUPLICATEASSETHEREACTION_H
