#ifndef SUNPACKCLIPSACTION_H
#define SUNPACKCLIPSACTION_H

#include "app/actions/saction.h"
#include "tw/graph/tw303aenv.h"
#include <QList>

// Action: the exact inverse of pack-clips (proposal 41 M2 AC2.2) -- a placed
// lane-fragment asset becomes its child clips again, restored to the ORIGINAL
// lane at the ORIGINAL times.
//
//   <unpack-clips clip="0,3"/>
//
// `clip` addresses the PLACEMENT (the asset clip on its lane) exactly as
// `remove-asset-placement`'s trackPath+clipIdx does, folded into one qualified
// path. Everything else is DERIVED from the object graph rather than stored:
// the asset name is the cut's own sName (pack-clips always sets it, exactly as
// create-asset does for a container asset), and each restored clip's absolute
// time is `placement.startTime + child.startTime` -- true by construction,
// because pack-clips places its result exactly at the group's own start
// (D2's "one placement where the material was"). So a clip moved between pack
// and unpack restores relative to wherever the placement NOW sits, which is
// the sensible reading of "unpack where I am" and happens to be identical to
// "the original lane at the original times" whenever nothing else touched the
// placement in between (AC2.2's round trip).
//
// REFUSES (AC2.4b, the sharing invariant): unpacking would move every child
// out of the shared fragment, which would leave any OTHER placement of the
// same asset windowing an emptied-out container. Checked via
// SObject::refCount() -- exactly 2 (the registry pin + this one placement)
// means no other placement exists; anything else is refused with a log line
// naming the asset.
//
// Inverse: SPackClipsAction over the clips' post-restore paths (mirrors
// SMoveClipAction: derive the inverse from where things ended up), reusing
// the SAME asset name so a redo produces byte-identical structure.
class SUnpackClipsAction : public SAction {
public:
    SUnpackClipsAction() = default;
    explicit SUnpackClipsAction( const QList<int> &clipPath );

    QString name() const override { return QStringLiteral( "unpack-clips" ); }
    QStringList knownAttributes() const override
    { return { QStringLiteral( "clip" ) }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> clipPath_;
};

#endif // SUNPACKCLIPSACTION_H
