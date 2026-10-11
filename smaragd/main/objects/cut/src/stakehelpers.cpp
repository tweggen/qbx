#include "app/objects/cut/stakehelpers.h"
#include "app/objects/cut/stakestack.h"
#include "app/model/sclipwindow.h"
#include "app/model/slink.h"
#include "app/model/sproject.h"
#include "app/persistence/sprojectloader.h"
#include "tw/core/twlog.h"
#include <QVector>
#include <QString>
#include <utility>
#include <vector>
#include "app/model/sclipwindow.h"

namespace stakes {


// Carry a PLACEMENT's own attributes onto a replacement link (plan 53 B1).
//
// Wrapping and collapsing both destroy the clip's SLink and build a new one.
// The old code copied `startTime` only, and a fresh SLink takes its timebase
// from SLink::defaultTimebaseFor( object ), which keys off contentKind() --
// audio is pinned to time, events follow the beat. So an EXPLICIT `beats`
// choice on an audio clip was silently reset to `time` by the wrap, and the
// clip then ignored every later set-tempo. eventChannelOverride_ went the same
// way, back to -1.
//
// THE ORDER MATTERS, and not obviously:
//
//   startTime  first -- setStartTime() itself calls syncTicksFromTime_(), so
//              this also leaves the ticks in step for a Time link.
//   timebase   second -- switching TO beats calls syncTicksFromTime_() again,
//              deriving ticks from the frame position just set. Were the ticks
//              set before this, the switch would overwrite them.
//   startTicks third -- the exact Fraction, authoritative for a Beats link,
//              whose setter re-derives startTime from it. This is what keeps a
//              tick value with no exact frame representation exact across the
//              wrap; the frame round trip above would have rounded it.
//
// A Time link keeps its ticks in step too, deliberately: SLink documents them
// as "meaningful only for a Beats link (a Time link keeps it in step so a
// later switch has something to start from)".
static void carryLinkAttrs( const SLink &from, SLink *to )
{
    if( !to ) return;
    to->setStartTime( from.getStartTime() );
    to->setTimebase( from.getTimebase() );
    to->setStartTicks( from.getStartTicks() );
    to->setEventChannelOverride( from.getEventChannelOverride() );
}

STakeStack *columnOfLink( SLink *lk )
{
    if( !lk ) return nullptr;
    SObject &obj = lk->getSObject();
    if( STakeStack *stack = dynamic_cast<STakeStack *>( &obj ) ) return stack;
    if( SClipWindow *win = SClipWindow::of( &obj ) )
        return dynamic_cast<STakeStack *>( &win->windowContent() );
    return nullptr;
}

bool isDirectColumn( SLink *lk )
{
    if( !lk ) return false;
    return dynamic_cast<STakeStack *>( &lk->getSObject() ) != nullptr;
}

void publishColumnChange( SLink *link, STakeStack *column )
{
    if( !link || !column ) return;
    SObject &obj = link->getSObject();
    if( &obj == (SObject *) column ) return;     // DIRECT: the track is wired
    if( SClipWindow *win = SClipWindow::of( &obj ) )
        win->setDurationFromTimeline( win->durationBlocking() );
}

namespace {

// Every placement in the project, depth first. Collected BEFORE anything is
// changed: the pass replaces lane child links, and mutating a child list while
// walking it is how the other link-swappers would have gone wrong too.
void collectPlacements( SObject &obj, QVector<SLink *> &out )
{
    for( SLink *lk : obj.childLinks() ) {
        if( !lk ) continue;
        out.append( lk );
        // Recurse only into things that CONTAIN placements — lanes and path
        // containers. Descending into a clip would collect its CONTENT link
        // as well, and this pass DELETES placements: the content link of a
        // wrapper collapsed a moment earlier would then be a dangling pointer
        // later in the same walk.
        SObject &child = lk->getSObject();
        if( child.isLane() || child.isPathContainer() )
            collectPlacements( child, out );
    }
}

// Is the wrapper's window an EXACT fold into the takes? Refused rather than
// approximated -- see the header.
bool foldIsExact( SClipWindow &wrapper, STakeStack &column, QString &why )
{
    if( wrapper.loopLength() != 0 )        { why = "the wrapper LOOPS"; return false; }
    if( wrapper.stretchOrRate() != Fraction( 1 ) )
                                           { why = "the wrapper is STRETCHED"; return false; }
    if( wrapper.asObject().getPan() != 0.0 )
                                           { why = "the wrapper is PANNED"; return false; }
    if( wrapper.asObject().hasAutomationLanes() )
                                           { why = "the wrapper carries AUTOMATION"; return false; }
    for( int i = 0; i < column.nTakes(); ++i ) {
        SClipWindow *t = column.takeAt( i );
        if( !t ) { why = "a take is missing"; return false; }
        if( t->loopLength() != 0 ) { why = "a TAKE loops"; return false; }
    }
    return column.nTakes() > 0;
}

}   // namespace

int normalizeColumns( SProject &project )
{
    if( qEnvironmentVariable( "SMARAGD_TAKE_MIGRATE" ).compare(
            QStringLiteral( "off" ), Qt::CaseInsensitive ) == 0 )
        return 0;

    SObject *root = project.getRootComponent();
    if( !root ) return 0;

    QVector<SLink *> placements;
    collectPlacements( *root, placements );

    int collapsed = 0, refused = 0;
    for( SLink *lk : placements ) {
        if( !lk ) continue;
        SObject *lane = dynamic_cast<SObject *>( lk->parent() );
        if( !lane ) continue;
        SClipWindow *wrapper = SClipWindow::of( &lk->getSObject() );
        if( !wrapper ) continue;                       // already DIRECT, or not a column
        STakeStack *column =
            dynamic_cast<STakeStack *>( &wrapper->windowContent() );
        if( !column ) continue;

        QString why;
        if( !foldIsExact( *wrapper, *column, why ) ) {
            ++refused;
            TW_LOGW( "cut", "take column left WRAPPED: %s",
                     why.toUtf8().constData() );
            continue;
        }

        // FOLD the wrapper's window into every take. The anchor goes through
        // the TAKE's own map, so a take that is stretched or warped lands on
        // the content position the wrapper actually showed.
        const Fraction wrapAnchor = wrapper->contentAnchorExact();
        const length_t wrapDur    = wrapper->durationBlocking();
        const double   wrapVolDb  = wrapper->asObject().getVolume();
        for( int i = 0; i < column->nTakes(); ++i ) {
            SClipWindow *t = column->takeAt( i );
            if( !t ) continue;
            t->setWindowExact( t->timelineToSourceExact( wrapAnchor ),
                               wrapDur, 0, t->stretchOrRate() );
            // dB SUM: exactly how twGainStage already composes a clip's
            // static gain with its curve (mix/CONTRACT.md inv. 21).
            if( wrapVolDb != 0.0 )
                t->asObject().setVolume( t->asObject().getVolume() + wrapVolDb );
        }

        // Replace the placement, reference-safe and index-stable, exactly as
        // wrapCutLinkIntoStack / collapseSingleTakeStack do: the new link takes
        // its ref on the column BEFORE the old link (and with it the wrapper,
        // and with that the wrapper's ref on the column) goes away.
        const int origIndex = lane->indexOfChild( lk );
        SObject *wrapObj = &wrapper->asObject();
        // The broad form, as in wrapCutLinkIntoStack -- see carryLinkAttrs.
        QObject::disconnect( wrapObj, nullptr, lane, nullptr );
        SLink *direct = new SLink( *column, nullptr );
        carryLinkAttrs( *lk, direct );
        delete lk;
        direct->setParent( lane );
        lane->moveChildToIndex( lane->indexOfChild( direct ), origIndex );
        // DELETE THE WRAPPER NOW, not by refcount. `removeRef()` schedules a
        // `deleteLater()`, and `SProject::serialize` writes every SObject that
        // is still a CHILD of the project -- so a wrapper waiting on the event
        // loop is written into the very file this pass exists to clean, still
        // holding its own reference to the column (measured: the saved column
        // read `nRefs='2'` with a dead `SCut` beside it). Qt removes a pending
        // DeferredDelete in ~QObject, so an explicit delete after one is safe,
        // and the refcount test is what makes it safe HERE: nothing else can
        // be holding the wrapper, because a wrapped column has exactly one
        // placement and it was the link just deleted.
        if( wrapObj->refCount() <= 0 ) delete wrapObj;
        ++collapsed;
    }
    if( collapsed || refused )
        TW_LOGI( "cut", "take columns normalised: %d collapsed, %d left wrapped",
                 collapsed, refused );
    return collapsed;
}

void splitCompMap( const twCompMap &src, long long at,
                   twCompMap *head, twCompMap *tail )
{
    std::vector<twCompSegment> headSegs, tailSegs;
    if( !src.empty() ) {
        // The take sounding AT the split opens the tail. Without this the tail
        // falls back to activeTake_ until its first inherited boundary and
        // plays the wrong take there -- the defect this is half of.
        const int atSplit = src.takeAt( at );
        if( atSplit >= 0 ) {
            tailSegs.push_back( twCompSegment{ 0, atSplit, 0 } );
        }
        for( const twCompSegment &sg : src.segments() ) {
            if( sg.at < at ) {
                headSegs.push_back( sg );
            } else if( sg.at > at ) {
                tailSegs.push_back( twCompSegment{ sg.at - at, sg.take,
                                                   sg.xfade } );
            }
            // sg.at == at is already the opening segment above, so it is
            // deliberately dropped here rather than duplicated at 0.
        }
    }
    if( head ) *head = twCompMap( std::move( headSegs ) ).normalized();
    if( tail ) *tail = twCompMap( std::move( tailSegs ) ).normalized();
}

STakeStack *cloneColumn( SProject *project, STakeStack &column )
{
    if( !project || column.nTakes() <= 0 ) return nullptr;
    STakeStack *copy = new STakeStack( project );
    for( int i = 0; i < column.nTakes(); ++i ) {
        SClipWindow *take = column.takeAt( i );
        if( !take ) continue;
        SClipWindow *dup = take->cloneWindowOver( project );
        if( !dup ) continue;
        if( !copy->insertTake( *dup ) ) delete &dup->asObject();
    }
    if( copy->nTakes() <= 0 ) { delete copy; return nullptr; }
    copy->setActiveTake( column.activeTakeIndex() < copy->nTakes()
                             ? column.activeTakeIndex() : 0 );
    // THE COMP MAP COMES TOO (plan 53 B3). Verbatim, and that is right for
    // every caller of this function: duplicate-clip, the view's Ctrl-drag and
    // split-clip's WRAPPED branch all keep the column's own frame domain, which
    // a window does not re-base. Only the DIRECT split re-bases, and it uses
    // splitCompMap instead.
    //
    // ONLY when every take cloned. A skipped take shifts every index above it,
    // which would silently re-point the map at the wrong material -- worse than
    // losing it. Announced, never silent.
    if( !column.compMap().empty() ) {
        if( copy->nTakes() == column.nTakes() ) {
            copy->setCompMap( column.compMap() );
        } else {
            TW_LOGW( "cut",
                     "cloneColumn: dropping the comp map -- %d of %d takes "
                     "cloned, so every segment above the missing one would "
                     "name the wrong take.",
                     copy->nTakes(), column.nTakes() );
        }
    }
    return copy;
}

SLink *wrapCutLinkIntoStack( SProject *project, SObject *lane,
                             SLink *cutLink )
{
    if( !project || !lane || !cutLink ) return nullptr;
    SClipWindow *win = SClipWindow::of( &cutLink->getSObject() );
    if( !win ) return nullptr;
    SObject &winObj = win->asObject();

    STakeStack *stack = new STakeStack( project );
    if( !stack->insertTake( *win ) ) {  // refs it BEFORE the old link dies
        delete stack;                   // cannot be refused into an empty stack
        return nullptr;
    }
    stack->setActiveTake( 0 );        // the wrapped material stays audible

    // The lane connected the window's signals when its link was added; deleting
    // the link does not sever an object-level connection. From now on the
    // window reports to the stack, the stack to the lane.
    //
    // THE BROAD FORM on purpose (plan 53 B1): STrack::trackChildWasAdded wires
    // TWO object-level signals -- durationChanged, and eventsChanged for
    // Event-kind content -- and the narrow durationChanged spelling missed the
    // second on an event column. Nothing connects lane -> object, so this
    // severs exactly those, and re-linking re-makes them through setParent ->
    // childEvent -> childObjectAdded -> trackChildWasAdded. SRemoveTrackAction
    // and SRemoveSampleAction's pin use the same form for the same reason.
    QObject::disconnect( &winObj, nullptr, lane, nullptr );

    SLink *stackLink = new SLink( *stack, nullptr );
    carryLinkAttrs( *cutLink, stackLink );
    const int origIndex = lane->indexOfChild( cutLink );
    delete cutLink;                   // lane removeClip fires
    stackLink->setParent( lane );     // lane insertClip fires (keyed anew)
    // setParent appends; restore the original index so clip paths recorded
    // in actions/inverses stay valid across the wrap (undo determinism).
    lane->moveChildToIndex( lane->indexOfChild( stackLink ), origIndex );
    return stackLink;
}

SLink *collapseSingleTakeStack( SObject *lane, SLink *stackLink )
{
    if( !lane || !stackLink ) return nullptr;
    STakeStack *stack = dynamic_cast<STakeStack *>( &stackLink->getSObject() );
    if( !stack || stack->nTakes() != 1 ) return nullptr;
    SObject *take = stack->takeObjectAt( 0 );
    if( !take ) return nullptr;

    // Sever the stack's per-take forwarding before it is orphaned. The broad
    // form, as in wrapCutLinkIntoStack: the stack forwards durationChanged AND
    // eventsChanged for an event column, and the narrow spelling missed the
    // second (plan 53 B1).
    QObject::disconnect( take, nullptr, stack, nullptr );

    SLink *cutLink = new SLink( *take, nullptr );  // ref before the stack dies
    carryLinkAttrs( *stackLink, cutLink );
    const int origIndex = lane->indexOfChild( stackLink );
    delete stackLink;                 // stack unreferenced → deleteLater
    cutLink->setParent( lane );
    // Keep the column at its original child index (path stability, as above).
    lane->moveChildToIndex( lane->indexOfChild( cutLink ), origIndex );
    return cutLink;
}

}  // namespace stakes

// Register the pair with the model's generic take-COLUMN factory, so a slice
// that may not depend on objects/cut (objects/midi, at the same rank) can
// still turn a plain placement into a column and collapse it back. A static
// initializer, exactly like SClipWindow::registerWrapFactory in scut.cpp -
// and it works for the same reason: `smaragd_app` is an OBJECT library, so
// nothing strips a translation unit whose only reference is a static ctor.
// The take-column normalisation pass (proposal 42 M4), registered the same
// way and for the same layering reason as the wrap factory below.
static const bool s_reg_take_normalise = (
    SProjectLoader::registerPostLoadPass(
        []( SProject &p ) { (void) stakes::normalizeColumns( p ); } ),
    true );

static const bool s_reg_take_column = (
    SClipWindow::registerTakeColumnFactory( &stakes::wrapCutLinkIntoStack,
                                            &stakes::collapseSingleTakeStack ),
    true );
