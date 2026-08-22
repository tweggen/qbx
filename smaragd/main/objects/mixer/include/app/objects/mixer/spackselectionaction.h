#ifndef SPACKSELECTIONACTION_H
#define SPACKSELECTIONACTION_H

#include "app/actions/saction.h"
#include <QList>
#include <QString>

// Action: pack a MULTI-LANE clip selection, one fragment PER LANE (proposal
// 41 D1/D2/D8).
//
//   <pack-selection clips="0,0;0,1;1,0;1,1;1,2;2,0"/>
//
// The partition rule, and it is the whole verb:
//
//     for each lane in the selection:
//         if it holds TWO OR MORE selected clips -> pack them (pack-clips)
//         otherwise                              -> leave the clip alone
//
// This is what the arranger's "Pack clips into fragment" context-menu item
// means over a selection that spans lanes. `pack-clips` is single-lane BY
// CONTRACT (AC2.5 / D8 -- it REFUSES a two-lane selection, naming both lanes,
// and fragment_pack_multilane_refused.qxa gates that refusal), so widening it
// with a mode flag would blunt the one gate that keeps a fragment single-lane.
// This verb sits ABOVE it instead: it partitions, and delegates every actual
// pack to an unmodified SPackClipsAction inside ONE SCompositeAction -- the
// same "a planner verb is a composite of primitives" shape place-recording
// and duplicate-asset-here already use, which is what makes the whole
// multi-lane pack ONE undo step.
//
// There is deliberately NO `name` attribute. One name cannot serve N lanes,
// and pack-clips' own empty-name path already generates the first unused
// "<lane name> N" per lane -- so two lanes both called "Audio" come out as
// "Audio 1" and "Audio 2" (each pack registers its asset before the next one
// runs, so the generator sees it).
//
// Refuses, mutating nothing (SCompositeAction rolls back a failed member):
// a selection in which NO lane holds two or more clips -- there is nothing to
// pack and an applied-but-empty action would put a no-op on the undo stack --
// and anything an individual pack-clips refuses (a dangling path, a clip
// whose parent is not a lane, a reference cycle).
//
// Inverse: the composite of the members' own inverses, i.e. one
// SUnpackClipsAction per fragment minted, in reverse order.
class SPackSelectionAction : public SAction {
public:
    SPackSelectionAction() = default;
    explicit SPackSelectionAction( const QList<QList<int>> &clipPaths );

    QString name() const override { return QStringLiteral( "pack-selection" ); }
    QStringList knownAttributes() const override
    { return { QStringLiteral( "clips" ) }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<QList<int>> clipPaths_;
};

// The partition rule as a FUNCTION, so the arranger's menu and the action
// share ONE definition of it.
//
// The menu has to answer "is there anything to pack here?" to decide whether
// to enable its item, and the action has to answer "what exactly do I pack?".
// Those are the same question, and proposal 41 M7 already paid for what
// happens when paint and hit-test each derive the same rule separately (the
// z-order bug that shipped green for two milestones). Purely syntactic: a
// clip path's lane is its path minus the last element, so this needs no
// project and no resolution.
namespace spackselection {

/** The selection grouped by owning lane path, lanes in first-seen order and
 *  each lane's clips in the order they appear in `clipPaths`. */
QList<QList<QList<int>>> groupByLane( const QList<QList<int>> &clipPaths );

/** How many of those groups hold two or more clips -- i.e. how many fragments
 *  pack-selection would mint. 0 means "nothing to pack". */
int packableLaneCount( const QList<QList<int>> &clipPaths );

}  // namespace spackselection

#endif // SPACKSELECTIONACTION_H
