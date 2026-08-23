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
    stack->setActiveTake( takeIndex_ );
    // `select-take` is "this take for the WHOLE column", so the comp map --
    // which says which take sounds WHERE -- is cleared; the inverse below
    // carries it back, or one undo of a click would destroy a comp silently.
    // Restoring it is what `prevMap_` is for, and on the forward pass that is
    // an empty map, i.e. the clear.
    stack->setCompMap( prevMap_ );
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
