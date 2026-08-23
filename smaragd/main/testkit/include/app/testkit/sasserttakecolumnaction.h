#ifndef _SASSERTTAKECOLUMNACTION_H_
#define _SASSERTTAKECOLUMNACTION_H_

#include "app/actions/saction.h"
#include <QString>

/**
 * `assert-take-column` — THE SHAPE GATE the take subsystem never had.
 *
 * Proposal 42: a take column reaches a track in two structural spellings,
 * DIRECT (`SLink -> STakeStack`) and WRAPPED (`SLink -> SCut/SMidiCut ->
 * STakeStack`), and 31 of the 43 sites that resolve one handle only the first.
 * Nothing designs the wrapped shape — three things PRODUCED it (a border
 * gesture, `duplicate-clip`, "add link"), and every consumer that forgot it
 * exists shipped looking exactly as correct as one that did not.
 *
 * No existing verb can see any of this. `assert-clip-window` reads the LINK's
 * own object, so it cannot distinguish a column from a window over one, and
 * its `take=` attribute is SILENTLY IGNORED on the wrapped shape (the generic
 * seam does not compose through a window). `assert-recorded-clip`'s take count
 * resolves DIRECT-only and reports 1 for a wrapped column.
 *
 * Attributes, each checked only when given:
 *   `clip`        the placement, as everywhere else (`"0,0"`, asset-qualified)
 *   `shape`       "direct" | "wrapped" | "none" — the STRUCTURAL assertion
 *   `takes`       expected take count
 *   `activeTake`  expected active index (-1 = none active)
 *   `comp`        the COMP MAP as text: `at:take[:xfade]` segments separated
 *                 by `;`, or the empty string for "no map" (the degenerate
 *                 state that means `activeTake_` everywhere). THE ONLY way a
 *                 script can see the map — proposal 43 N1 gives it no audible
 *                 effect, so model state is all there is to assert.
 *   `takeAtPos`   a column position, paired with `expectTake`: what
 *                 `takeIndexAt()` answers there, WITH the degenerate case
 *                 folded in. That is the query every future consumer makes,
 *                 so it is the one worth gating rather than the raw map.
 *   `expectTake`  see above
 *   `placements`  expected refcount of the COLUMN object. A column belongs to
 *                 exactly ONE placement (proposal 42); two placements share one
 *                 `activeTake_`, so comping either comps both — which is
 *                 indistinguishable from comping not working.
 *
 * Not undoable: an assertion is not an edit.
 */
class SAssertTakeColumnAction : public SAction
{
public:
    SAssertTakeColumnAction() {}
    SApplyResult apply( SProject *project ) override;
    QString name() const override
    { return QStringLiteral( "assert-take-column" ); }
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString clip_;
    QString shape_;
    int takes_ = -1;
    int activeTake_ = -2;      // -2 = not asserted (-1 IS a legal value)
    int placements_ = -1;
    QString comp_;
    bool hasComp_ = false;
    qint64 takeAtPos_ = -1;
    int expectTake_ = -2;
};

#endif // _SASSERTTAKECOLUMNACTION_H_
