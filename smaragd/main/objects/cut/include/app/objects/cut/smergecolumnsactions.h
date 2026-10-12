#ifndef SMERGECOLUMNSACTIONS_H
#define SMERGECOLUMNSACTIONS_H

#include "app/actions/saction.h"
#include "tw/events/twcompmap.h"
#include "tw/graph/tw303aenv.h"
#include "tw/core/twfraction.h"
#include "app/model/slink.h"
#include <QList>
#include <QString>

class SObject;
class SClipWindow;
class STakeStack;
class SUnmergeColumnsAction;

/**
 * `merge-columns` — FOLD SEVERAL LANE CHILDREN INTO ONE UNION COLUMN
 * (plan 53 §6.1, Q14; the verb mode A needs).
 *
 * `clips="0,1;0,2;0,4"`, one lane at a time — a selection spanning lanes is
 * run once per lane by the UI. The result is ONE column at the FIRST operand's
 * child index, with extent `[min start, max end)`:
 *
 *  - every PLAIN clip becomes a take at its own extent;
 *  - every COLUMN's takes transfer at their extents shifted by
 *    `(colStart − newStart)`, and its comp map likewise;
 *  - the combined map is each operand's previous answer over its own extent,
 *    the LATER operand in the list winning where two overlap;
 *  - the active take is the last operand's active take (or the plain clip
 *    itself).
 *
 * THE WINDOWS ARE MOVED, NOT COPIED, so fades, names, volume, pan, automation
 * and the link attributes (`timebase_`, `startTicks_`, `eventChannelOverride_`)
 * travel with each take for free — the identity rule plan 52 established, and
 * the reason `stakes::moveTakeBetweenColumns` exists.
 *
 * A GAP BETWEEN OPERANDS BECOMES A SPAN NO TAKE COVERS, which under invariant
 * 1′ plays SILENCE (plan 53 §3.1 step 5) — exactly what the lane played there
 * before. That is why this verb depends on A0a: without extents a merge would
 * have to stretch some take across the gap and would change what plays.
 *
 * IT IS THE ONE VERB IN THE SET THAT REMOVES. It keeps its first operand's
 * index and removes the others, so every later child index shifts DOWN — the
 * reason `srecplan::ChildList` models an index SPACE rather than a list
 * (`objects/cut/CONTRACT.md`). Removals inside one merge are issued in
 * DESCENDING index order, so no removal invalidates an index still to be used.
 *
 * REFUSED, AND ANNOUNCED IN EVERY CASE (a refusal that can also happen by
 * accident has to be gated on the announcement, not on the action failing):
 * fewer than two operands; operands on different lanes; a clip that is a lane;
 * a WRAPPED column (`SLink -> SCut -> STakeStack`, which proposal 42 M3's "no
 * column inside a column" forbids folding); a live recording; mixed content
 * kinds; and — see below — operands whose mix parameters disagree.
 *
 * ## THE PARAMETER REFUSAL (not in §6.1; see CONTRACT.md and the PR)
 *
 * §6.1 says "fades, names, volume, pan ... are properties of the window or its
 * link", and for a take that is true. But the MIX reads a placement's
 * parameters through `SClipWindow::parametersOf`, which for a DIRECT column
 * resolves to its ACTIVE TAKE — so a merged column has exactly ONE set of
 * clip parameters (gain curve, static volume, fade, pan) and the mix applies
 * them to the WHOLE column. N operands with different levels cannot all be
 * honoured, which would make the merge change what plays.
 *
 * Plan 53's own acceptance criterion for this row is that the render is
 * IDENTICAL before and after. So where identity cannot hold, this verb
 * refuses instead of quietly re-mixing the lane: every operand must agree on
 * `getVolume()` and `getPan()`, and none may carry a `cut:Gain` curve or a
 * clip fade — a curve and a fade are POSITIONAL (a fade's length is measured
 * against the clip's own end, `twClipFade::gainAt`), so even identical ones
 * would mean something different once the clip is four times longer.
 *
 * This is QBX-185 in a second place: per-take gain is applied ABOVE the
 * column, so it cannot vary per take. When QBX-185 is fixed the refusal can
 * narrow to the cases that genuinely cannot be carried.
 */
class SMergeColumnsAction : public SAction {
public:
    SMergeColumnsAction() = default;
    explicit SMergeColumnsAction( const QList<QList<int>> &clipPaths );
    ~SMergeColumnsAction() override;

    // Holding refcount pins makes this action non-copyable: a copy would
    // release the same references twice (SRemoveSampleAction, same reason).
    SMergeColumnsAction( const SMergeColumnsAction & ) = delete;
    SMergeColumnsAction &operator=( const SMergeColumnsAction & ) = delete;

    QString name() const override { return QStringLiteral( "merge-columns" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

    /**
     * ONE OPERAND'S RECIPE: everything `unmerge-columns` needs to put that
     * child back exactly where and as it was.
     *
     * Takes are identified by their window OBJECT, never by an index. The
     * inverse moves takes out of the merged column one at a time and every
     * removal shifts the indices above it, so an index recorded here would be
     * stale by the second move; a pointer cannot be.
     */
    struct Operand {
        bool       wasColumn  = false;
        int        origIndex  = -1;      ///< child index on the lane, pre-merge
        offset_t   origStart  = 0;       ///< its link's startTime
        length_t   origDur    = 0;       ///< its object's own duration
        // The link attributes that were on the LANE link (not on a take's).
        // Captured as the link's own types, not flattened: `startTicks_` is a
        // Fraction (exact beat position) and rounding it through an integer
        // here would move a beats-timebase clip on every undo.
        SLink::Timebase timebase = SLink::Timebase::Time;
        Fraction        startTicks;
        int             eventChannelOverride = -1;
        // PLAIN: the one window that became a take.
        // COLUMN: the windows that became takes, in the column's own take
        // order, so the inverse restores that order as well as the extents.
        QList<SObject *> takeObjects;
        QList<offset_t>  takeStarts;     ///< each take's extent start, pre-merge
        QList<length_t>  takeLens;
        // COLUMN only: the column object itself, PINNED (see the header).
        STakeStack *column     = nullptr;
        bool        holdsColumnRef = false;
        twCompMap   columnMap;
        int         columnActive = -1;
    };

    /** The recipe, borrowed by the inverse through a back-pointer. */
    const QList<Operand> &operands() const { return operands_; }
    const QList<int>     &lanePath() const { return lanePath_; }
    const QString        &pathRoot() const { return pathRoot_; }

    /** Drop the column pins, after the inverse has re-linked them. */
    void releasePins();
    /**
     * Release pins left by a PREVIOUS apply. Called first on every apply, for
     * the reason `SRemoveSampleAction::dropStalePin` exists: a redo re-applies
     * this action against NEW objects and would otherwise leak one pin per
     * redo.
     */
    void dropStalePins();

private:
    QString          pathRoot_;
    QList<QList<int>> clipPaths_;
    QList<int>       lanePath_;
    QList<Operand>   operands_;
};

/**
 * `unmerge-columns` — the live-only inverse of `merge-columns` (plan 53 §6.1).
 *
 * It holds the recipe and RE-LINKS THE SAME WINDOW OBJECTS, which is plan 52's
 * identity rule: a take carries its own window, fades, name, volume, pan and
 * automation, and rebuilding it from a captured list drops whatever the list
 * forgot. The forward pins nothing for the TAKES — they are children of the
 * merged column and the inverse moves them back — but it DOES pin each operand
 * COLUMN, which §6.1 says is unnecessary and the code says otherwise: an
 * `STakeStack` is an `SObject`, so it carries its own `volume`, `pan`, `muted`,
 * `solo` and `recordingChannels` (`SObject::serializeSelfAttributes`), none of
 * which is on a take or on a link. Rebuilding the column would silently drop
 * them, which is the precise failure plan 52 exists to prevent.
 *
 * It restores every original child index with `moveChildToIndex`, in ASCENDING
 * original index, so an inverse owes child ORDER back and not merely child
 * existence (QBX-149: a redo of the merge would otherwise resolve the same
 * positional index to a different clip).
 *
 * LIVE-ONLY: it carries a pointer to a live action and live `SObject`s, which
 * no XML can express. `writeXml` is empty and `readXml` returns false, exactly
 * as `SRestoreTakeColumnAction` and `SRestoreContainerClipAction` are. The
 * FORWARD stays serializable, just as `remove-track` does while
 * `restore-track` does not.
 */
class SUnmergeColumnsAction : public SAction {
public:
    SUnmergeColumnsAction() = default;
    explicit SUnmergeColumnsAction( SMergeColumnsAction *owner );

    QString name() const override
        { return QStringLiteral( "unmerge-columns" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    SMergeColumnsAction *owner_ = nullptr;   // holds the pins; borrowed
};

#endif // SMERGECOLUMNSACTIONS_H
