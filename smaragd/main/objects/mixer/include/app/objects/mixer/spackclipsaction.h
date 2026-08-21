#ifndef SPACKCLIPSACTION_H
#define SPACKCLIPSACTION_H

#include "app/actions/saction.h"
#include "tw/graph/tw303aenv.h"
#include <QList>
#include <QString>

// Action: a same-lane clip SELECTION becomes one SLaneFragment + one
// registered SCut + one placement where the material was (proposal 41 D1/D2,
// M2 AC2.1).
//
//   <pack-clips clips="0,0;0,1;0,2" name="Riff"/>
//
// `clips` is a semicolon-separated list of qualified clip paths (each the
// same spelling `assert-clip-window`/`move-clip` use: a lane's index-path
// plus the link index within it), sharing at most ONE root qualifier
// ("Drums:0,0;0,1" -- the qualifier travels once, at the front, like
// `qualifiedToString`'s single-root convention). Every clip must resolve to
// the SAME lane (D8: a fragment is single-lane by construction) -- a
// selection spanning two lanes is REFUSED, naming both (AC2.5).
//
// The new fragment's children are the MOVED clips themselves (identity
// preserved, not copies), relocated from their absolute timeline positions to
// fragment-relative ones (shifted by the selection's own earliest start), so
// the registered cut's window is exactly [0, groupEnd-groupStart) and its
// PLACEMENT lands at that earliest start -- "one placement where the material
// was" (AC2.1). `name` is optional; empty generates `<lane name> N`.
//
// Refuses (never mutates on refusal): an empty/dangling clip path, a
// selection spanning two lanes (AC2.5), or a selection whose own content
// would close a reference cycle if placed back on its lane -- reusing
// `sarrangements::reaches` verbatim (AC2.6), checked BEFORE anything moves.
//
// Inverse: SUnpackClipsAction (below), whose own inverse re-packs -- the
// exact mirror of SMoveClipAction's "derive the inverse from where things
// ended up" discipline, so pack/unpack round-trip with no separate
// snapshot of "where things used to be".
class SPackClipsAction : public SAction {
public:
    SPackClipsAction() = default;
    SPackClipsAction( const QList<QList<int>> &clipPaths,
                      const QString &assetName = QString() );

    QString name() const override { return QStringLiteral( "pack-clips" ); }
    QStringList knownAttributes() const override
    { return { QStringLiteral( "clips" ), QStringLiteral( "name" ) }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<QList<int>> clipPaths_;
    QString            assetName_;   // empty => generate
};

#endif // SPACKCLIPSACTION_H
