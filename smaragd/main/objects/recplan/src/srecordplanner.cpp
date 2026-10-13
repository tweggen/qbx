#include "app/objects/recplan/srecordplanner.h"
#include "app/actions/saction.h"
#include "app/actions/scompositeaction.h"
#include "app/model/sautomationlane.h"
#include "app/model/sclipwindow.h"
#include "app/model/slink.h"
#include "app/model/sobject.h"
#include <QtGlobal>
#include <algorithm>

namespace srecplan {

// ------------------------------------------------------------------ modes --

Mode modeFromString( const QString &mode )
{
    if( mode == QLatin1String( "takes-split" ) ) return Mode::TakesSplit;
    if( mode == QLatin1String( "overdub" ) )     return Mode::Overdub;
    if( mode == QLatin1String( "replace" ) )     return Mode::Replace;
    // "takes", "" and anything unknown. See the header: empty means "whatever
    // this build would do anyway", and since P2 that is mode A.
    return Mode::Takes;
}

QString modeToString( Mode mode )
{
    switch( mode ) {
        case Mode::TakesSplit: return QStringLiteral( "takes-split" );
        case Mode::Overdub:    return QStringLiteral( "overdub" );
        case Mode::Replace:    return QStringLiteral( "replace" );
        case Mode::Takes:      break;
    }
    return QStringLiteral( "takes" );
}

// ------------------------------------------------------------- ChildList --

ChildList::ChildList( const LaneSnapshot &snap )
    : lanePath_( snap.lanePath )
{
    // ONE SLOT PER REAL LANE CHILD, not one per CLASSIFIED child. `snapshot()`
    // leaves out a live recording and a child with no duration, so iterating
    // `snap.children` would collapse those gaps and shift every later index
    // down by one -- and a path computed from it would address a real child,
    // just the wrong one. See LaneSnapshot::laneChildCount.
    for( int i = 0; i < snap.laneChildCount; ++i )
        slots_.append( { true, i } );
}

int ChildList::append()
{
    const int id = nextAppendId_++;
    slots_.append( { false, id } );
    return id;
}

bool ChildList::removeAt( int currentIndex )
{
    if( currentIndex < 0 || currentIndex >= slots_.size() ) return false;
    slots_.removeAt( currentIndex );
    return true;
}

int ChildList::indexOf_( bool original, int key ) const
{
    for( int i = 0; i < slots_.size(); ++i )
        if( slots_[i].original == original && slots_[i].key == key ) return i;
    return -1;
}

int ChildList::indexOfOriginal( int snapshotIndex ) const
{ return indexOf_( true, snapshotIndex ); }

int ChildList::indexOfAppended( int id ) const
{ return indexOf_( false, id ); }

QList<int> ChildList::pathOfOriginal( int snapshotIndex ) const
{
    const int i = indexOfOriginal( snapshotIndex );
    if( i < 0 ) return {};
    QList<int> p = lanePath_;
    p.append( i );
    return p;
}

QList<int> ChildList::pathOfAppended( int id ) const
{
    const int i = indexOfAppended( id );
    if( i < 0 ) return {};
    QList<int> p = lanePath_;
    p.append( i );
    return p;
}

// -------------------------------------------------------------- snapshot --

LaneSnapshot snapshot( SObject *lane, const QList<int> &lanePath )
{
    LaneSnapshot snap;
    snap.lanePath = lanePath;
    if( !lane ) return snap;
    snap.laneChildCount = lane->childCount();

    for( int i = 0; i < lane->childCount(); ++i ) {
        SLink *lk = lane->childAt( i );
        if( !lk ) continue;
        SObject &obj = lk->getSObject();

        // A GROWING CLIP IS NOT THERE as far as a plan is concerned, whichever
        // recorder owns it. See the header.
        if( obj.isLiveRecording() ) continue;

        Child c;
        c.index       = i;
        c.start       = lk->getStartTime();
        c.contentKind = obj.contentKind();

        if( obj.isLane() ) {
            // A sub-track. Reported rather than skipped so a planner can say
            // "this lane holds sub-tracks" if it ever needs to; no planner
            // places anything on one.
            c.kind = Child::Lane;
            snap.children.append( c );
            continue;
        }
        if( !obj.hasDuration() ) continue;
        // Blocking read, as the old planner did (P19): the whole gap/take plan
        // is derived from these durations, and a stale try-lock value
        // mis-plans it.
        c.dur = obj.getDurationBlocking();

        // THE MIX PARAMETERS, through the question `merge-columns` asks: on a
        // DIRECT column `parametersOf` resolves to the ACTIVE take, which is
        // the set the mix actually applies. See `Child::mixVolume`.
        c.hasWindow = SClipWindow::of( &obj ) != nullptr;
        if( SClipWindow *par = SClipWindow::parametersOf( obj ) ) {
            SObject &po = par->asObject();
            c.mixVolume = po.getVolume();
            c.mixPan    = po.getPan();
            SAutomationLane *gain =
                po.automationLane( QStringLiteral( "cut:Gain" ) );
            c.hasGainCurve = gain && !gain->isEmpty();
            c.hasFade      = !po.clipFade().none();
        }

        // THE COLUMN CLASSIFICATION, IN MODEL TERMS ONLY (QBX-188). It used
        // to ask `stakes::columnOfLink` / `isDirectColumn`, which name
        // `STakeStack` — out of this module's reach now that it sits at the
        // rank of `objects/cut` rather than inside it. The three questions are
        // exactly equivalent through the kind-agnostic accessors:
        //
        //   a column        obj.windowTakeCount() > 0
        //   DIRECT          ...and the link's own object is not a window
        //   WRAPPED         ...and it is  (SLink -> SCut -> column)
        //
        // because `SCut` FORWARDS windowTakeCount/windowTakeAt/
        // activeWindowTakeIndex/hasCompMap to its content, which is what makes
        // the wrapped shape answer for the column it windows.
        if( obj.windowTakeCount() > 0 ) {
            c.nTakes      = obj.windowTakeCount();
            c.activeTake  = obj.activeWindowTakeIndex();
            c.hasCompMap  = obj.hasCompMap();
            const bool wrapped = SClipWindow::of( &obj ) != nullptr;
            c.kind = wrapped          ? Child::WrappedColumn
                   : !c.hasCompMap    ? Child::Column
                                      : Child::MappedColumn;
        } else {
            c.kind = Child::Plain;
        }
        snap.children.append( c );
    }
    return snap;
}

namespace {

/**
 * The children a plan may treat as COLUMNS for this pass, in time order.
 *
 * `OtherKind` is decided here rather than in `snapshot()`, because which kind
 * is "other" is the PASS's question, not the lane's: an event clip is other
 * kind to an audio recording and the same clip is the point of a MIDI one.
 */
QList<Child> columnsFor( const LaneSnapshot &snap, const Span &span,
                         const Context &ctx )
{
    QList<Child> out;
    for( const Child &c : snap.children ) {
        if( c.kind == Child::Lane ) continue;
        if( c.dur == 0 ) continue;
        if( c.end() <= span.a || c.start >= span.b ) continue;
        // AN OTHER-KIND CHILD IS R0 (plan 53 B2, QBX-162). This pass places
        // one kind of material, and a clip of the other kind can never
        // receive it as a TAKE -- a take is an alternative for the same
        // region, not a different instrument.
        if( c.contentKind != ctx.kind ) {
            Child o = c;
            o.kind = Child::OtherKind;
            out.append( o );
            continue;
        }
        out.append( c );
    }
    // Stable, and by start: the plan walks the pass left to right.
    std::stable_sort( out.begin(), out.end(),
                      []( const Child &x, const Child &y )
                      { return x.start < y.start; } );
    return out;
}

/**
 * MODE `overdub`: the pass is a plain clip over whatever the lane holds, and
 * both sound (plan 53 §3.3).
 *
 * The whole planner is one `place-clip`. That was the point of it in P1a: the
 * proof that the framework could express a mode the planner of the day could
 * not, with no model change and no new verb, and it is the one mode for which
 * "what the lane already holds" is genuinely irrelevant -- so it reads the
 * snapshot for nothing but the lane path.
 */
class Overdub : public Planner
{
public:
    bool plan( const LaneSnapshot &snap, const Span &span, const Context &ctx,
               SCompositeAction &out, QString *why ) const override
    {
        if( !ctx.armed ) return true;
        if( span.length() == 0 ) {
            if( why ) *why = QStringLiteral( "empty span" );
            return false;
        }
        // NO VERB SET IS A REFUSAL, not an empty plan: an empty composite
        // applies as SUCCESS, which is how a recording goes missing silently.
        if( !ctx.verbs ) {
            if( why ) *why = QStringLiteral( "no verb set for this content "
                                             "kind" );
            return false;
        }
        out.append( ctx.verbs->placePass( snap.lanePath, span, span.a,
                                          span.length() ) );
        return true;
    }
};

/**
 * CAN `split-clip` ACTUALLY CUT THIS PIECE AT `t`?
 *
 * `SSplitClipAction::apply` refuses `inObjOffset <= 1` and
 * `inObjOffset >= fullDur - 1`, so a cut needs TWO frames of material on each
 * side -- not merely a position strictly inside the piece, which is how plan
 * 53 §3.2 words it ("split at `a` and at `b` where those fall strictly
 * inside").
 *
 * The difference is not cosmetic. A planner that emits a split one frame
 * inside a clip gets `{false, nullptr}` back from the sub-action, and
 * `SCompositeAction::apply` then rolls the WHOLE pass back and returns false
 * -- so the take is lost, which is the one thing a recorder may not do. A cut
 * that close is therefore not attempted: the cell degrades to the no-split
 * one, which always has an answer (the piece keeps its own edge, and the
 * recording's take anchor carries the ≤ 2-frame difference as the leading
 * silence a negative `srcOffset` already expresses, proposal 23).
 *
 * The pass-wide snap (§3.5) removes almost every occurrence of this before a
 * planner ever sees it -- a landing within `N` of a clip edge is moved ONTO
 * the edge -- but only when every armed lane agrees. With lanes disagreeing
 * there is no snap, and a landing one frame inside a clip is reachable.
 */
bool pieceCanSplitAt( offset_t pieceStart, length_t pieceDur, offset_t t )
{
    const offset_t o = t - pieceStart;
    return o >= 2 && o <= (offset_t) pieceDur - 2;
}

/**
 * THE PIECE OF ONE TOUCHED CHILD THAT LIES INSIDE THE PASS, isolated by
 * `split-clip` so that nothing outside `[a, b)` is touched.
 *
 * This is the half of plan 53 §3.2 and §3.3 that both new planners share:
 * `takes-split` then hands the piece the pass as a TAKE, `replace` REMOVES it.
 * The splitting itself is identical, and so is the index bookkeeping it needs.
 *
 * `id` is how the caller addresses the piece afterwards: < 0 means "still the
 * original child, by its snapshot index", >= 0 means "the child `append()`
 * returned this id for".
 */
struct InsidePiece {
    offset_t start = 0;
    length_t dur   = 0;
    int      id    = -1;
};

/**
 * Emit the splits that isolate `c`'s inside piece, and model them.
 *
 * SPLIT-CLIP'S INDEX CONTRACT, which this depends on: the HEAD keeps the
 * operand's index and the TAIL is APPENDED. True on both shapes a lane
 * placement can have -- the direct-column branch keeps `link` and appends
 * `sl2`, and the general branch keeps `cutLink` (which IS `link` whenever the
 * object is already a clip window) and appends `sl2`.
 *
 * `ssplitclipaction.cpp` has a THIRD path, for a link whose object is raw
 * content with no window: it deletes the link and appends a wrapped
 * replacement, so the head's index MOVES. No lane placement reaches it --
 * `place-clip`, `add-sample` and `place-recording` all create `SCut` windows,
 * a direct take column is an `STakeStack` handled by the first branch, and
 * `place-asset`'s body link is either an `SCut` or a container that
 * `snapshot()` reports as `Child::Lane` and no planner places on. Stated here
 * rather than guarded, because a guard would need a `hasWindow` flag on
 * `Child` that nothing could make true, and the assumption is one grep from
 * being re-checked.
 *
 * EVERY SPLIT GOES OUT WITH `broadcast=0` (plan 53 §5.1). An edit group would
 * otherwise fan the cut out to the members' corresponding clips, and a
 * recording pass decides the edit for the lanes the RECORDER armed -- which it
 * plans one by one, each against its own snapshot. Letting `split-clip`
 * broadcast as well would cut lanes this pass never looked at, and cut an
 * armed member twice.
 */
InsidePiece isolateInside( const Child &c, const Span &span,
                           const PassVerbs &verbs,
                           ChildList &live, SCompositeAction &out )
{
    InsidePiece p;
    p.start = c.start;
    p.dur   = c.dur;
    p.id    = -1;

    auto path = [&]() {
        return p.id < 0 ? live.pathOfOriginal( c.index )
                        : live.pathOfAppended( p.id );
    };

    // The pass STARTS inside this child: cut at `a`, keep the head as it was,
    // and the appended TAIL is the piece the pass covers.
    if( c.start < span.a && pieceCanSplitAt( p.start, p.dur, span.a ) ) {
        out.append( verbs.splitClip( path(), span.a ) );
        p.id    = live.append();
        p.dur   = (length_t)( p.start + (offset_t) p.dur - span.a );
        p.start = span.a;
    }
    // The pass ENDS inside what is left: cut at `b`. The piece keeps its own
    // index this time and the TAIL AFTER `b` is the appended one.
    if( p.start + (offset_t) p.dur > span.b
        && pieceCanSplitAt( p.start, p.dur, span.b ) ) {
        out.append( verbs.splitClip( path(), span.b ) );
        live.append();
        p.dur = (length_t)( span.b - p.start );
    }
    return p;
}

/**
 * A WRAPPED COLUMN REFUSES THE WHOLE LANE for this pass (plan 53 G11, the
 * matrix's "any R · Kw" row, §7 B6's report).
 *
 * It is a POLICY now and no longer a forced hand, which is worth saying
 * because the plan's reason has expired. §3.3 and §12 said `replace` must
 * refuse a Kw lane because `remove-sample` could restore only the direct
 * shape; QBX-155 landed (#241, #244) and it now pins BOTH shapes, so that
 * particular obstacle is gone. What keeps the refusal is the rule itself: a
 * surviving wrapper is non-foldable (`stakes::normalizeColumns` ran at load
 * and left it), every mode would have to split, fold or delete it, and none
 * has a gate that has ever exercised one. Refusing is announced, costs the
 * pass nothing but this lane, and is what `record_lane_skipped_reported`
 * (QBX-164) is waiting for. **Since P2 this reaches the DEFAULT mode**, which
 * is a flip: the planner before it handed a wrapper to `add-take` like any
 * other column.
 */
bool refusedForWrappedColumn( const QList<Child> &touched, Mode mode,
                              QString *why )
{
    for( const Child &c : touched ) {
        if( c.kind != Child::WrappedColumn ) continue;
        if( why )
            *why = QStringLiteral( "the lane holds a wrapped take column at "
                                   "%1, which mode '%2' refuses (plan 53 G11)" )
                       .arg( (long long) c.start )
                       .arg( modeToString( mode ) );
        return true;
    }
    return false;
}

/**
 * CAN `merge-columns` TAKE THIS SET OF OPERANDS? Asked from the SNAPSHOT,
 * before a single verb is emitted.
 *
 * WHY MODE A PRE-FLIGHTS A REFUSAL IT COULD SIMPLY RUN INTO. `merge-columns`
 * decides before mutating and announces its reason, which is exactly right for
 * a user gesture: nothing happens and the user is told why. For a recording it
 * is fatal. `SCompositeAction::apply` rolls back every earlier child on the
 * first one that fails, so the pass's own `place-clip` goes with it and the
 * take is LOST -- the one thing a recorder may not do (the same rule that made
 * `takes-split` guard `split-clip` with `pieceCanSplitAt`).
 *
 * The rules mirrored here are the verb's own, in its order. Everything else it
 * refuses needs the live tree (a path that does not resolve, a lane, a live
 * recording) and `place-clip` would have failed on it first, or is impossible
 * by construction (fewer than two operands, a clip named twice, mixed content
 * kinds -- `columnsFor` has already sent the other kind to R0).
 *
 * THE PARAMETER RULE IS THE ONE THAT ACTUALLY BITES. A merged column has ONE
 * set of clip parameters, read from its active take, so the verb refuses
 * operands that disagree on volume or pan, or that carry anything POSITIONAL
 * (a `cut:Gain` envelope, a clip fade) -- QBX-185. The pass is a FRESH
 * `place-clip`, which means volume 0, pan 0, no fade and no envelope, so
 * "the operands agree with each other and with the pass" is simply "every
 * operand is at the defaults".
 */
bool mergeableOperands( const QList<Child> &ops, const Span &span,
                        QString *why )
{
    for( const Child &c : ops ) {
        if( !c.isColumn() && !c.hasWindow ) {
            if( why )
                *why = QStringLiteral( "the child at %1 is neither a windowed "
                                       "clip nor a take column" )
                           .arg( (long long) c.start );
            return false;
        }
        if( c.isColumn() && c.nTakes <= 0 ) {
            if( why )
                *why = QStringLiteral( "the take column at %1 is empty" )
                           .arg( (long long) c.start );
            return false;
        }
        if( c.hasGainCurve ) {
            if( why )
                *why = QStringLiteral( "the child at %1 carries a cut:Gain "
                                       "envelope, which is POSITIONAL and "
                                       "would mean something else in a longer "
                                       "column (QBX-185)" )
                           .arg( (long long) c.start );
            return false;
        }
        if( c.hasFade ) {
            if( why )
                *why = QStringLiteral( "the child at %1 carries a clip fade, "
                                       "whose length is measured against its "
                                       "own end" )
                           .arg( (long long) c.start );
            return false;
        }
        if( c.mixVolume != 0.0 || c.mixPan != 0.0 ) {
            if( why )
                *why = QStringLiteral( "the child at %1 plays at %2 dB / pan "
                                       "%3 and the pass at 0 / 0; a merged "
                                       "column has ONE level for every take "
                                       "(QBX-185)" )
                           .arg( (long long) c.start )
                           .arg( c.mixVolume ).arg( c.mixPan );
            return false;
        }
    }
    (void) span;
    return true;
}

/**
 * MODE `takes` (A): ONE UNION COLUMN PER PASS PER LANE (plan 53 §3.1).
 *
 * This is the row the whole plan was built for, and it REPLACES the planner
 * every build before it had (`LegacyTakes`, QBX-180 / P2). Under it a
 * recording never lands beside the material it was played against: the touched
 * children and the pass become takes of one column spanning
 * `C = [ min(a, min s_i), max(b, max e_i) )`, the pass is the active take and
 * the comp map answers with the pass inside `[a, b)` and with what was there
 * before everywhere else.
 *
 * THE PLAN IS TWO VERBS, and that is the point of A1 having landed first:
 *
 *   place-clip   the pass, at `a`, over its extent
 *   merge-columns  every touched child IN ASCENDING CHILD INDEX, then the pass
 *
 * `merge-columns` (§6.1, QBX-179) already IS §3.1 steps 1, 2 and 4: the result
 * sits at the first operand's index with extent `[min start, max end)`, every
 * plain clip becomes a take at its own extent, a column's takes and comp map
 * transfer shifted, and the combined map is each operand's PREVIOUS ANSWER
 * over its own extent with the LATER operand winning an overlap and the
 * previous answer re-entering at each operand's end. Naming the pass LAST
 * therefore gives step 3 for free -- it wins inside `[a, b)` and becomes the
 * active take -- and the rest of the map is `M0` by construction.
 *
 * NO GAP FILLING, AND NOT BY OVERSIGHT. Every member of `T` intersects
 * `[a, b)`, so the one child that can start before `a` must also end after it
 * and therefore covers `[C.start, a)` entirely; the same holds at the other
 * end, and the pass covers `[a, b)` itself. `C` is contiguous AND fully
 * covered -- §3.1's "contiguous by construction" -- so unlike `LegacyTakes`
 * and `TakesSplit` this planner has no uncovered sub-span to place a cut over.
 *
 * ASCENDING CHILD INDEX IS D1's TOPMOST RULE (§3.1 step 4): where two touched
 * children overlap, the previous answer is the one with the HIGHER child
 * index, and `merge-columns` gives an overlap to the LATER operand -- so the
 * list has to be in index order, not the time order `columnsFor` returns.
 * D1 (a) also means the SUM those two children used to produce is GONE: a
 * column plays one take per position. `rec_union_overlap_legacy` asserts that,
 * so the decision is visible in the suite rather than silent.
 *
 * THE PASS IS PLACED OVER ITS EXTENT, NOT ITS MATERIAL (§3.1 step 5): for a
 * cycle pass `Span::extent` is the whole cycle, so the clip -- and hence the
 * take -- spans it even when the performer stopped early, and the take is an
 * alternative for the same bars as the full passes beside it.
 *
 * > **A DEVIATION FROM §3.1 STEP 4, where `extent` reaches past `b`.** Step 4
 * > says the map is `M0` outside `[a, b)`, but `merge-columns` paints an
 * > operand's answer over its whole EXTENT, so the pass answers over
 * > `[a, extentEnd)` and renders silence from `b` on instead of falling back
 * > to the old material. Separating the two would need a verb that sets a
 * > take's extent independently of the span it answers for, and none exists --
 * > `add-take at=/length=` is the only writer of an extent and it creates a
 * > take. The combination is reachable only by a live CYCLE pass that stops
 * > mid-cycle over pre-existing material, and the behaviour it produces is
 * > defensible on its own: the user asked for a pass over those bars, that
 * > pass is the answer for all of them, silence included -- which is what a
 * > tape machine does -- and the previous answer is one `select-take` away.
 * > Recorded as a plan 53 correction rather than coded around.
 *
 * A WRAPPED COLUMN REFUSES THE LANE, as under the other two new modes (G11):
 * `merge-columns` cannot fold one without dropping the wrapper's own window.
 * This is a FLIP -- `LegacyTakes` handed a wrapper to `add-take` like any
 * other column -- and it is the plan's decision (§3.3's "any R · Kw" row);
 * the refusal is announced and the file is reported unused (B6/Q13).
 */
class Takes : public Planner
{
public:
    bool plan( const LaneSnapshot &snap, const Span &span, const Context &ctx,
               SCompositeAction &out, QString *why ) const override
    {
        if( !ctx.armed ) return true;          // nothing to do (review 2 M5)
        if( span.length() == 0 ) {
            if( why ) *why = QStringLiteral( "empty span" );
            return false;
        }
        // NO VERB SET IS A REFUSAL, not an empty plan: an empty composite
        // applies as SUCCESS, which is how a recording goes missing silently.
        if( !ctx.verbs ) {
            if( why ) *why = QStringLiteral( "no verb set for this content "
                                             "kind" );
            return false;
        }

        const QList<Child> touched = columnsFor( snap, span, ctx );
        if( refusedForWrappedColumn( touched, Mode::Takes, why ) )
            return false;

        QList<Child> ops;
        for( const Child &c : touched ) {
            if( c.kind == Child::Lane ) continue;
            // AN OTHER-KIND CHILD IS R0 and keeps everything (plan 53 B2): a
            // take is an alternative for the same region, not a different
            // instrument. The pass is laid OVER it, which is what an empty
            // lane does and what never loses audio.
            if( c.kind == Child::OtherKind ) continue;
            if( !c.isColumn() && c.kind != Child::Plain ) continue;
            ops.append( c );
        }
        std::stable_sort( ops.begin(), ops.end(),
                          []( const Child &x, const Child &y )
                          { return x.index < y.index; } );

        QString no;
        const bool merging = !ops.isEmpty()
                             && mergeableOperands( ops, span, &no );
        if( !ops.isEmpty() && !merging ) {
            // DEGRADED, NOT REFUSED, and this is the one plan that changes no
            // existing child: the user's faded or re-levelled clip keeps its
            // fade and its level, and the pass is audible over it. A
            // LegacyTakes-shaped degrade would instead WRAP that clip into a
            // column, where `parametersOf` resolves to the ACTIVE take and the
            // clip's own parameters stop applying -- the degrade would itself
            // change what plays, which is what the refusal exists to prevent.
            // Flattening the fade and running `merge-columns` by hand is the
            // way back, and QBX-185 is the way out.
            qWarning( "place-recording: mode 'takes' placed the pass as a "
                      "plain clip rather than a union column: %s",
                      qPrintable( no ) );
        }

        ChildList live( snap );
        out.append( ctx.verbs->placePass(
            snap.lanePath, span, span.a,
            (length_t)( span.extentEnd() - span.a ) ) );
        const int passId = live.append();

        // An empty lane region is today's plain placement, exactly (§3.1's
        // "for a lane whose touched set is empty"), and so is a degrade.
        if( !merging ) return true;

        QList<QList<int>> paths;
        for( const Child &c : ops )
            paths.append( live.pathOfOriginal( c.index ) );
        paths.append( live.pathOfAppended( passId ) );
        out.append( ctx.verbs->mergeColumns( paths ) );
        // NOTHING FOLLOWS, so the merge's removals need no modelling: it is
        // the last verb in the plan, and `place-recording` re-snapshots for
        // every segment of every pass.
        return true;
    }
};

/**
 * MODE `takes-split` (B): the pass is an ALTERNATIVE FOR EXACTLY THE SPAN IT
 * COVERS, so nothing is stacked outside it (plan 53 §3.2).
 *
 * Every touched child is cut at `a` and at `b` where those fall inside it; the
 * piece inside `[a, b)` becomes a column with the old piece as take 0 and the
 * pass as take 1, active; the pieces outside are untouched; the sub-spans of
 * `[a, b)` that no child covers become plain cuts. Invariant 1 therefore holds
 * as written today -- a take is an alternative for the column's whole extent,
 * which is why this mode needs none of §4's take-extent model change and can
 * land before it.
 *
 * TWO THINGS HAPPEN BY THEMSELVES, both because the prerequisites landed
 * first, and both deliberately NOT re-implemented here:
 *
 *  - the inside piece's comp map is CLEARED when the pass is activated
 *    (`add-take activate=1`, QBX-159/B4), which is what §3.2's "the inside
 *    piece's map is cleared when R is activated" asks for;
 *  - a split column CARRIES its map, head and tail (QBX-160/B3,
 *    `stakes::splitCompMap` on the direct shape and `cloneColumn` on the
 *    wrapped one).
 *
 * OVERLAPPING CHILDREN GO LEFT TO RIGHT, which is a DEVIATION from §3.4 and is
 * recorded there. §3.4's mode-B bullet says the TOPMOST inside piece receives
 * the pass; that is well defined only when one piece contains another, and for
 * a partial overlap it names no answer for the part the topmost piece does not
 * cover -- which would lose recorded audio. The cursor rule below gives the
 * pass to the FIRST piece that covers each position and places it exactly
 * once, which agrees with §3.4 wherever §3.4 is precise (W1's adjacent
 * children) and never loses audio where it is not. "Topmost" was decision D1,
 * and P2 took option (a): mode A merges such a lane and gives an overlap to
 * the HIGHEST child index. Mode B keeps the cursor rule, which is its own
 * answer to a question mode A does not have -- B never makes one column out
 * of two children.
 */
class TakesSplit : public Planner
{
public:
    bool plan( const LaneSnapshot &snap, const Span &span, const Context &ctx,
               SCompositeAction &out, QString *why ) const override
    {
        if( !ctx.armed ) return true;          // nothing to do (review 2 M5)
        if( span.length() == 0 ) {
            if( why ) *why = QStringLiteral( "empty span" );
            return false;
        }
        // NO VERB SET IS A REFUSAL, not an empty plan: an empty composite
        // applies as SUCCESS, which is how a recording goes missing silently.
        if( !ctx.verbs ) {
            if( why ) *why = QStringLiteral( "no verb set for this content "
                                             "kind" );
            return false;
        }

        const QList<Child> touched = columnsFor( snap, span, ctx );
        if( refusedForWrappedColumn( touched, Mode::TakesSplit, why ) )
            return false;

        ChildList live( snap );
        offset_t cursor = span.a;

        for( const Child &c : touched ) {
            if( c.kind == Child::Lane ) continue;
            // AN OTHER-KIND CHILD IS R0 and keeps everything, span included
            // (plan 53 B2): a take is an alternative for the same region, not
            // a different instrument. The pass is not clipped around it
            // either -- it falls through to a plain cut laid OVER it, which is
            // what an empty lane does and what never loses audio.
            if( c.kind == Child::OtherKind ) continue;

            const offset_t os = std::max( c.start, span.a );
            const offset_t oe = std::min( c.end(), span.b );
            if( oe <= os ) continue;
            // Already covered by an earlier child in this same pass: the
            // overlap rule above, and the same "one recording, one column"
            // reason as B5 -- handing the pass to a second piece would make it
            // sound twice.
            if( os < cursor ) continue;

            // The sub-span no child covers becomes a plain cut.
            if( os > cursor ) {
                out.append( ctx.verbs->placePass(
                    snap.lanePath, span, cursor,
                    (length_t)( os - cursor ) ) );
                live.append();
            }

            const InsidePiece p = isolateInside( c, span, *ctx.verbs, live,
                                                out );
            const QList<int> piecePath =
                p.id < 0 ? live.pathOfOriginal( c.index )
                         : live.pathOfAppended( p.id );

            // The pass becomes take 1 of the inside piece, active. Its anchor
            // is where the PIECE starts, not where the pass does: when a cut
            // at `a` was refused as too close to the child's own start the
            // piece begins up to two frames EARLY, and the anchor then goes
            // slightly negative -- leading silence, legal since proposal 23.
            out.append( ctx.verbs->addTakeFromPass( piecePath, span,
                                                    p.start ) );
            // add-take PRESERVES its operand's index (it replaces the lane
            // child's link in place), so the simulation needs no change --
            // stated rather than silent, because it is the one verb in either
            // plan that neither appends nor removes.

            cursor = std::max( cursor,
                               std::min( p.start + (offset_t) p.dur, span.b ) );
        }

        if( cursor < span.b ) {
            // The trailing sub-span, and on an empty lane this is the whole
            // pass at `a` -- the same plain placement every other mode makes.
            out.append( ctx.verbs->placePass(
                snap.lanePath, span, cursor,
                (length_t)( span.b - cursor ) ) );
            live.append();
        }
        return true;
    }
};

/**
 * MODE `replace` (tape): every touched child LOSES `[a, b)`, then the pass is
 * laid down as a plain cut (plan 53 §3.3). Never wraps, never stacks.
 *
 * EXPRESSED AS `split-clip` + `remove-sample`, NOT as the left-edge
 * `resize-clip` §3.3 and §12 name -- and the reason is the planner contract
 * rather than taste. A left-edge trim has to name the piece's new SOURCE
 * anchor, and the exact one is `SClipWindow::timelineToSourceExact( b - s )`,
 * which is a question only the LIVE window can answer: it runs through the
 * clip's warp map, so `srcStart + (b - s) * stretch` is wrong for any
 * warp-anchored clip. Rule 2 forbids a planner from reading the lane again,
 * and widening `Child` to carry `srcStart` plus `stretch` would carry exactly
 * the arithmetic that is wrong. `split-clip` already computes that anchor,
 * exactly, inside itself -- and `comp_map_survives_split` already gates it --
 * so cutting at `b` and removing the head produces the identical window with
 * no new arithmetic anywhere. The cells read:
 *
 *   R1 (`s < a`, `e <= b`)  cut at `a`, remove the TAIL      -> `[s, a)` left
 *   R2 (`s < a`, `e > b`)   cut at `a` and `b`, remove middle -> `[s,a)`+`[b,e)`
 *   R3/R5 (`a <= s`, `e <= b`)  remove the child outright
 *   R4/R6 (`a <= s`, `e > b`)   cut at `b`, remove the HEAD   -> `[b, e)` left
 *
 * which is one code path: isolate the inside piece exactly as `takes-split`
 * does, then remove that piece instead of adding a take to it.
 *
 * `remove-sample` IS THE RIGHT VERB and not merely an available one: it is the
 * arranger's own Delete, its inverse is pinned rather than rebuilt for a take
 * column (plan 52 / QBX-146 step a) and it decides invertibility BEFORE
 * mutating, so a shape it cannot restore refuses instead of deleting
 * silently. The planner passes an EMPTY file path on purpose -- the action
 * reads the real one off the clip, and its own comment says the caller's value
 * is "only a hint" -- but it must pass the piece's START, because that is the
 * position the inverse restores to.
 */
class Replace : public Planner
{
public:
    bool plan( const LaneSnapshot &snap, const Span &span, const Context &ctx,
               SCompositeAction &out, QString *why ) const override
    {
        if( !ctx.armed ) return true;
        if( span.length() == 0 ) {
            if( why ) *why = QStringLiteral( "empty span" );
            return false;
        }
        // NO VERB SET IS A REFUSAL, not an empty plan: an empty composite
        // applies as SUCCESS, which is how a recording goes missing silently.
        if( !ctx.verbs ) {
            if( why ) *why = QStringLiteral( "no verb set for this content "
                                             "kind" );
            return false;
        }

        const QList<Child> touched = columnsFor( snap, span, ctx );
        if( refusedForWrappedColumn( touched, Mode::Replace, why ) )
            return false;

        ChildList live( snap );

        for( const Child &c : touched ) {
            if( c.kind == Child::Lane ) continue;
            // R0, and under `replace` this is the one place the mode is NOT
            // destructive: an event clip keeps its notes while an audio pass
            // replaces the audio around it. Same rule as every other mode
            // (plan 53 B2), with more at stake.
            if( c.kind == Child::OtherKind ) continue;

            const offset_t os = std::max( c.start, span.a );
            const offset_t oe = std::min( c.end(), span.b );
            if( oe <= os ) continue;

            const InsidePiece p = isolateInside( c, span, *ctx.verbs, live,
                                                out );
            const int idx = p.id < 0 ? live.indexOfOriginal( c.index )
                                     : live.indexOfAppended( p.id );
            if( idx < 0 ) continue;          // cannot happen; not worth a crash

            out.append( ctx.verbs->removeChild( snap.lanePath, idx,
                                               p.start ) );
            live.removeAt( idx );
        }

        // The pass itself, over the hole every touched child just made.
        out.append( ctx.verbs->placePass( snap.lanePath, span, span.a,
                                          span.length() ) );
        live.append();
        return true;
    }
};

}  // namespace

const Planner &plannerFor( Mode mode )
{
    // FUNCTION-LOCAL statics, not file-scope ones: a planner is stateless, so
    // one instance can serve every armed lane of every pass, but a file-scope
    // object would be constructed in an order this file does not control and
    // `plannerFor` is callable from anywhere.
    static const Takes      takes;
    static const Overdub     overdub;
    static const TakesSplit  takesSplit;
    static const Replace     replace;

    switch( mode ) {
        case Mode::Overdub:    return overdub;
        case Mode::TakesSplit: return takesSplit;
        case Mode::Replace:    return replace;
        case Mode::Takes:      break;
    }
    return takes;
}

}  // namespace srecplan
