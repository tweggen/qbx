#include "app/objects/cut/sselecttakeaction.h"
#include "app/objects/cut/stakestack.h"
#include "app/objects/cut/stakehelpers.h"
#include "app/model/seditgroups.h"
#include "app/actions/scompositeaction.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/model/slink.h"
#include "app/actions/sactionregistry.h"
#include <QDomElement>
#include <utility>
#include <vector>

using namespace strackpath;

SSelectTakeAction::SSelectTakeAction( const QList<int> &clipPath,
                                      int takeIndex, bool broadcast )
    : clipPath_( clipPath ), takeIndex_( takeIndex ), broadcast_( broadcast )
{
}

SApplyResult SSelectTakeAction::apply( SProject *project )
{
    if( !project || clipPath_.isEmpty() ) {
        return {false, nullptr};
    }
    // Edit-group broadcast: same take index on every member's corresponding
    // stack. Members whose clip is not a stack (or lacks the take) are
    // skipped up front — a composite child rejection would roll the whole
    // edit back, but "as applicable" is the spec.
    if( broadcast_ ) {
        QList<QList<int>> targets =
            seditgroups::expandClipPaths( project, clipPath_ );
        if( targets.size() > 1 ) {
            SObject *root = splacements::rootNamed( project, pathRoot_ );
            SCompositeAction composite;
            for( const QList<int> &p : targets ) {
                SLink *lk = splacements::placementAt( root, p );
                STakeStack *st = stakes::columnOfLink( lk );
                if( !st || takeIndex_ >= st->nTakes() ) continue;
                composite.append(
                    new SSelectTakeAction( p, takeIndex_, false ) );
            }
            if( composite.count() > 0 )
                return composite.apply( project );
            return {false, nullptr};
        }
    }
    SObject *mixer = splacements::rootNamed( project, pathRoot_ );
    SLink *link = splacements::placementAt( mixer, clipPath_ );
    if( !link ) {
        return {false, nullptr};
    }
    // BOTH SHAPES. `columnOfLink` unwraps `SLink -> SCut -> STakeStack`, which
    // is what a SHARED or PLACED column is and what a real saved project
    // carries. The bare cast this replaced matched the DIRECT shape only, so a
    // click on a wrapped column's take lane submitted this action and had it
    // REFUSED -- the take-lane UI resolves through the wrapper (SMVActualView's
    // takeStackOfLink, now the same call) and the action did not, so the
    // gesture the UI could express was one the action would not accept.
    STakeStack *stack = stakes::columnOfLink( link );
    if( !stack ) {
        return {false, nullptr};       // not a take stack
    }
    if( takeIndex_ < -1 || takeIndex_ >= stack->nTakes() ) {
        return {false, nullptr};       // take index out of range
    }

    const int oldActive = stack->activeTakeIndex();
    const twCompMap oldMap = stack->compMap();

    // THE MAP `select-take` LEAVES BEHIND (plan 53 §4.3).
    //
    // Before extents, `select-take` meant "this take for the WHOLE column", so
    // the comp map -- which says which take sounds WHERE -- was simply cleared.
    // Under invariant 1' a take may not COVER the whole column, and clearing
    // the map there would make every position outside its extent fall back to
    // it and so play SILENCE: selecting a take would delete the others'
    // audio without touching them.
    //
    // So: select the take WHEREVER IT HAS MATERIAL. Segments inside
    // [tStart, tEnd) are replaced by one `tStart -> take`, the previous answer
    // RE-ENTERS at tEnd, and segments outside are kept.
    //
    // THE FULL-EXTENT CASE IS STILL THE CLEAR, and not merely as an
    // optimisation. A one-segment map `{0 -> take}` answers exactly what an
    // empty map plus `activeTake_ = take` answers, but it SERIALIZES: a
    // `select-take` on an ordinary un-comped column would start writing a
    // <comp> element into project files that never had one, which is the rule
    // that a new feature writes nothing when unused. The clear is the
    // canonical spelling of the same map.
    twCompMap newMap;
    const bool fullExtent = stack->takeIsFullExtent( takeIndex_ );
    if( restoreMap_ ) {
        // AN INVERSE. It restores the map it was handed, verbatim, whatever
        // the take it names covers -- see setPreviousCompMap.
        newMap = prevMap_;
    } else if( takeIndex_ >= 0 && !fullExtent ) {
        const offset_t exStart = stack->takeExtentStart( takeIndex_ );
        const offset_t exEnd   = stack->takeExtentEnd( takeIndex_ );
        const length_t colDur  = stack->getDurationBlocking();
        // What the column answered before this click, with the empty map's
        // meaning ("activeTake_ everywhere") spelled out -- the answer that has
        // to survive outside the new take's extent.
        const auto oldAnswerAt = [&oldMap, oldActive]( offset_t pos ) -> int {
            const int t = oldMap.takeAt( (int64_t) pos );
            return t >= 0 ? t : oldActive;
        };

        std::vector<twCompSegment> segs;
        bool hadAtStart = false, hadAtEnd = false;
        for( const twCompSegment &cs : oldMap.segments() ) {
            const offset_t at = (offset_t) cs.at;
            if( at == exStart ) hadAtStart = true;
            if( at == exEnd )   hadAtEnd   = true;
            if( at >= exStart && at < exEnd ) continue;   // inside: replaced
            segs.push_back( cs );                         // outside: kept
        }
        // The region BEFORE the extent kept its answer only because nothing
        // governed it and `activeTake_` was the fall-back. `activeTake_` is
        // about to change, so that answer has to be written down.
        if( exStart > 0 && oldMap.takeAt( 0 ) < 0 )
            segs.push_back( twCompSegment{ 0, oldActive, 0 } );
        // The take itself. An xfade already AT this boundary is kept: the
        // boundary survives the click, only the take it introduces changes.
        twCompSegment mine{ (int64_t) exStart, takeIndex_, 0 };
        for( const twCompSegment &cs : oldMap.segments() )
            if( (offset_t) cs.at == exStart ) mine.xfade = cs.xfade;
        segs.push_back( mine );
        // ...and the previous answer re-enters where the take's material ends,
        // unless an old segment already says so there, or the extent runs to
        // the column's end.
        if( exEnd < (offset_t) colDur && !hadAtEnd )
            segs.push_back( twCompSegment{ (int64_t) exEnd,
                                           oldAnswerAt( exEnd ), 0 } );
        (void) hadAtStart;
        newMap = twCompMap( std::move( segs ) ).normalized();
    }
    // else: TODAY'S CLEAR. A full-extent take sounds everywhere, so the empty
    // map says exactly what a one-segment map would and writes nothing.

    stack->setActiveTake( takeIndex_ );
    stack->setCompMap( newMap );
    stakes::publishColumnChange( link, stack );

    SSelectTakeAction *inverse = new SSelectTakeAction( clipPath_, oldActive );
    inverse->setPreviousCompMap( oldMap );
    return {true, inverse};
}

void SSelectTakeAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", qualifiedToString( pathRoot_, clipPath_ ) );
    elem.setAttribute( "take", takeIndex_ );
    elem.setAttribute( "broadcast", broadcast_ ? 1 : 0 );
}

bool SSelectTakeAction::readXml( const QDomElement &elem, int /*version*/ )
{
    clipPath_ = parseInto( pathRoot_, elem.attribute( "clip" ) );
    takeIndex_ = elem.attribute( "take", "-1" ).toInt();
    broadcast_ = elem.attribute( "broadcast", "1" ).toInt() != 0;
    return true;
}

static const bool s_reg_selecttake = (
    SActionRegistry::instance().registerType(
        QStringLiteral("select-take"),
        []{ return new SSelectTakeAction; }
    ), true
);
