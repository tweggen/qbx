#include "app/objects/cut/srecordplanner.h"
#include "app/objects/cut/saddtakeaction.h"
#include "app/objects/cut/splaceclipaction.h"
#include "app/objects/cut/stakehelpers.h"
#include "app/objects/cut/stakestack.h"
#include "app/actions/scompositeaction.h"
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
    // this build would do anyway", and that is the legacy planner.
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

        if( STakeStack *col = stakes::columnOfLink( lk ) ) {
            c.nTakes     = col->childCount();
            c.activeTake = col->activeTakeIndex();
            c.map        = col->compMap();
            c.kind = !stakes::isDirectColumn( lk ) ? Child::WrappedColumn
                   : c.map.empty()                 ? Child::Column
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
 * TODAY'S PLANNER, moved here verbatim in behaviour (plan 53 P1: `LegacyTakes`
 * behind the `takes` value, FLIPS NONE).
 *
 * The rule, unchanged: a column that starts WITH the recording takes the
 * matching segment as a new take; the gaps between them become plain windowed
 * cuts; a column that was already running at the recording start is dropped
 * from the plan entirely and the take is placed OVER it.
 *
 * TWO CLASSIFICATIONS ARE READ AS R0 HERE, both for the reason the old
 * planner gave: an OTHER-KIND child (plan 53 B2) and a child this build does
 * not recognise. They are left out of the column list, so the recording falls
 * through to the gap branch and is placed as its own clip over them. Losing
 * recorded audio is not something a recorder may do.
 *
 * A WRAPPED COLUMN (Kw) IS TREATED AS A COLUMN, which plan 53 §5.1 says every
 * planner refuses (G11). That sentence and P1's "Flips: none" cannot both hold
 * for this planner: today's planner hands a wrapped column to `add-take` like
 * any other, so refusing it here WOULD be a flip. The refusal is the new
 * planners' rule; `LegacyTakes` keeps the old behaviour until P2 retires it.
 */
class LegacyTakes : public Planner
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

        ChildList live( snap );
        const QList<int> lanePath = snap.lanePath;

        QList<Child> cols;
        for( const Child &c : columnsFor( snap, span, ctx ) ) {
            if( c.kind == Child::OtherKind ) continue;     // R0
            if( !c.isColumn() && c.kind != Child::Plain ) continue;
            if( c.start < span.a ) {
                // A column ALREADY RUNNING at the recording start is dropped
                // from the plan rather than kept in it. Keeping it is what
                // silently threw the take away: the plan skipped it but still
                // advanced the cursor past its end, so the column CONSUMED the
                // material it covered, and when it covered the whole recording
                // the composite came out EMPTY -- which applies as success.
                // qxa.record_stays_armed failed that way in roughly 1 run in 15.
                qWarning( "place-recording: column at %lld predates the "
                          "recording start; it keeps its span and the take is "
                          "placed over it", (long long) c.start );
                continue;
            }
            cols.append( c );
        }

        offset_t cursor = span.a;
        for( const Child &col : cols ) {
            // ONE RECORDING, ONE COLUMN (plan 53 B5, QBX-163). A column whose
            // start is already below the cursor has had its span covered by an
            // earlier column in this same pass; handing it the same recording
            // would make two columns hold one pass as an alternative for two
            // different regions, and `add-take` WRAPS a plain clip, so an
            // ordinary clip overlapping the first column would be silently
            // turned into a take column it was never meant to be.
            if( col.start < cursor ) continue;

            if( col.start > cursor ) {
                out.append( new SPlaceClipAction(
                    lanePath, span.filePath, cursor,
                    span.srcOffset + cursor - span.a,
                    (length_t)( col.start - cursor ) ) );
                live.append();
            }
            out.append( new SAddTakeAction(
                live.pathOfOriginal( col.index ), span.filePath,
                span.srcOffset + col.start - span.a ) );
            // add-take PRESERVES its operand's index (it replaces the lane
            // child link in place), so the simulation needs no change here --
            // stated rather than silent, because it is the one verb in the
            // plan that does not append.

            cursor = std::max( cursor, std::min( col.end(), span.b ) );
        }
        if( cursor < span.b ) {
            // The trailing gap; when nothing overlapped at all this is the
            // whole pass at `a` -- today's plain placement.
            out.append( new SPlaceClipAction(
                lanePath, span.filePath, cursor,
                span.srcOffset + cursor - span.a,
                (length_t)( span.b - cursor ) ) );
            live.append();
        }
        return true;
    }
};

/**
 * MODE `overdub`: the pass is a plain clip over whatever the lane holds, and
 * both sound (plan 53 §3.3).
 *
 * The whole planner is one `place-clip`. That is the point of it in P1a: it is
 * the proof that the framework can express a mode the legacy planner cannot,
 * with no model change and no new verb, and it is the one mode for which "what
 * the lane already holds" is genuinely irrelevant -- so it reads the snapshot
 * for nothing but the lane path.
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
        out.append( new SPlaceClipAction( snap.lanePath, span.filePath, span.a,
                                          span.srcOffset, span.length() ) );
        return true;
    }
};

/**
 * `takes-split` and `replace` are QBX-172's (plan 53 P1b).
 *
 * They REFUSE loudly rather than falling back to `LegacyTakes`. A fallback
 * would give a `.qxa` driving `mode="takes-split"` the legacy shape and no
 * indication that the mode did nothing -- precisely the clean, repeatable
 * false finding an unknown attribute produces, which is the failure mode this
 * project has already paid for once.
 */
class NotYetImplemented : public Planner
{
public:
    explicit NotYetImplemented( Mode mode ) : mode_( mode ) {}
    bool plan( const LaneSnapshot &, const Span &, const Context &,
               SCompositeAction &, QString *why ) const override
    {
        if( why )
            *why = QStringLiteral( "record mode '%1' is not implemented in "
                                   "this build (QBX-172)" )
                       .arg( modeToString( mode_ ) );
        return false;
    }
private:
    Mode mode_;
};

}  // namespace

const Planner &plannerFor( Mode mode )
{
    // FUNCTION-LOCAL statics, not file-scope ones: a planner is stateless, so
    // one instance can serve every armed lane of every pass, but a file-scope
    // object would be constructed in an order this file does not control and
    // `plannerFor` is callable from anywhere.
    static const LegacyTakes       takes;
    static const Overdub          overdub;
    static const NotYetImplemented takesSplit( Mode::TakesSplit );
    static const NotYetImplemented replace( Mode::Replace );

    switch( mode ) {
        case Mode::Overdub:    return overdub;
        case Mode::TakesSplit: return takesSplit;
        case Mode::Replace:    return replace;
        case Mode::Takes:      break;
    }
    return takes;
}

}  // namespace srecplan
