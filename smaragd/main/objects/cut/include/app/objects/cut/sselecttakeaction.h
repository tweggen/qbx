#ifndef SSELECTTAKEACTION_H
#define SSELECTTAKEACTION_H

#include "app/actions/saction.h"
#include "tw/events/twcompmap.h"
#include <QList>

// Action: select the audible take of a take stack (proposal 17) — the
// comping gesture. `clip` addresses the stack placement (track-path + link
// index), `take` is the take index (-1 = none audible). Rejected when the
// clip is not a take stack or the index is out of range.
//
// Inverse: select-take with the previously active index.
//
// `take="newest"` (plan 53 §6.5, Q10, QBX-182) is the one non-numeric value:
// the take that was RECORDED LAST, resolved PER COLUMN, so an edit-group
// broadcast lands on the pass just recorded on every member even where the
// members hold different numbers of takes. The inverse is always a plain
// index — undo restores a take, not a rule.
//
// `whole="1"` (QBX-186) is "HEAR THIS TAKE ALONE": the take sounds over the
// WHOLE column, whatever it covers. See the member for why that gesture had
// to be given a spelling of its own.
class SSelectTakeAction : public SAction {
public:
    SSelectTakeAction() = default;
    SSelectTakeAction( const QList<int> &clipPath, int takeIndex,
                       bool broadcast = true, bool newest = false,
                       bool whole = false );

    QString name() const override { return QStringLiteral("select-take"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

    /**
     * The COMP MAP to restore (proposal 43 N1), and the mark that THIS ACTION
     * IS AN INVERSE.
     *
     * `select-take` used to mean "this take for the WHOLE column", so it
     * cleared the map and its inverse had to put the map back, or one undo of
     * a click would silently destroy a comp. Since invariant 1' the forward
     * pass no longer always clears -- on a PARTIAL take it computes a map
     * (§4.3) -- so an inverse must be told apart from a forward pass rather
     * than inferred from the take it names. An inverse whose take happens to
     * be partial would otherwise recompute §4.3's map instead of restoring the
     * one it was handed, and undo would not be undo.
     *
     * Exactly one caller, the inverse this action builds, and it must keep
     * exactly one — the same discipline `SSetPluginParamAction::
     * setPreviousValue()` carries and for the same reason.
     */
    void setPreviousCompMap( const twCompMap &m )
    { prevMap_ = m; restoreMap_ = true; }

private:
    twCompMap prevMap_;
    /** Set only by setPreviousCompMap: restore `prevMap_` VERBATIM. */
    bool       restoreMap_ = false;
    QList<int> clipPath_;
    int        takeIndex_ = -1;
    /**
     * `take="newest"`: resolve the index from the COLUMN at apply time
     * (plan 53 §6.5).
     *
     * THE RULE IS "THE LAST RECORDED TAKE", and it is two steps: among the
     * takes whose name carries the recorder's stamp (`stakenames::isRecorded`)
     * take the HIGHEST INDEX; if no take carries one, the highest index of
     * all. §6.5's "a take without a stamp is older than any with one" is the
     * first step; the second is the fall-back for a column nobody recorded
     * into, where "newest" can only mean the last one added.
     *
     * THE ORDER COMES FROM THE INDEX, NOT FROM THE STAMP, and `stakenames.h`
     * says why: `HH:MM` has no date and two passes in one minute tie, while a
     * recorded take is always APPENDED (`merge-columns` names the pass as its
     * final operand, `add-take` appends), so the column's own take order IS
     * recording order.
     */
    bool       newest_ = false;
    /**
     * `whole="1"`: THIS TAKE OVER THE WHOLE COLUMN (QBX-186) — "hear this one
     * take alone".
     *
     * WHY IT NEEDS A FLAG AT ALL. Before extents this is what a plain
     * `select-take` did, and clearing the map was the only way to get it.
     * §4.3 made the plain form extent-aware for a good reason (clearing the
     * map on a PARTIAL take makes every span it does not cover play SILENCE,
     * so selecting a take would delete the other takes' audio without
     * touching one of them) — and put nothing in the vacated place. For a
     * FULL-EXTENT take the plain form still clears, so the loss was invisible
     * while recording produced full-extent takes. Under mode A (§3.1, P2,
     * QBX-180) takes are PARTIAL BY CONSTRUCTION, which is the whole point of
     * the union, so from P2 onward no gesture isolated a take at all. The
     * measure of the hole: four A0a gates were built on the old side effect
     * and all four had to be repaired with `set-comp-map` — a scripting verb,
     * not a gesture. They are written with this flag now.
     *
     * IT CLEARS THE MAP, and that is not an optimisation. A one-segment map
     * `{0 -> take}` answers exactly what an empty map plus `activeTake_`
     * answers, but it SERIALIZES: a `<comp>` element would start appearing in
     * project files that never had one, against the rule that a new feature
     * writes nothing when unused (§4.3 correction (g)). The clear is the
     * canonical spelling of the same map.
     *
     * The INVERSE is a plain restoring `select-take`, never a `whole` one:
     * `setPreviousCompMap` puts the previous map back verbatim, which is the
     * only thing that can undo a clear.
     */
    bool       whole_ = false;
    // Edit groups: comp the SAME take index on every member's corresponding
    // stack (drum-kit comping); members without that take are skipped.
    bool       broadcast_ = true;
};

#endif // SSELECTTAKEACTION_H
