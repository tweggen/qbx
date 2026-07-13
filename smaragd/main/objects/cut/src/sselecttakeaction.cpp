#include "app/objects/cut/sselecttakeaction.h"
#include "app/objects/cut/stakestack.h"
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
            SObject *root = splacements::rootContainer( project );
            SCompositeAction composite;
            for( const QList<int> &p : targets ) {
                SLink *lk = splacements::placementAt( root, p );
                STakeStack *st = lk ? dynamic_cast<STakeStack*>(
                                          &lk->getSObject() ) : nullptr;
                if( !st || takeIndex_ >= st->nTakes() ) continue;
                composite.append(
                    new SSelectTakeAction( p, takeIndex_, false ) );
            }
            if( composite.count() > 0 )
                return composite.apply( project );
            return {false, nullptr};
        }
    }
    SObject *mixer = splacements::rootContainer( project );
    SLink *link = splacements::placementAt( mixer, clipPath_ );
    if( !link ) {
        return {false, nullptr};
    }
    STakeStack *stack = dynamic_cast<STakeStack *>( &link->getSObject() );
    if( !stack ) {
        return {false, nullptr};       // not a take stack
    }
    if( takeIndex_ < -1 || takeIndex_ >= stack->nTakes() ) {
        return {false, nullptr};       // take index out of range
    }

    const int oldActive = stack->activeTakeIndex();
    stack->setActiveTake( takeIndex_ );

    SAction *inverse = new SSelectTakeAction( clipPath_, oldActive );
    return {true, inverse};
}

void SSelectTakeAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", pathToString( clipPath_ ) );
    elem.setAttribute( "take", takeIndex_ );
    elem.setAttribute( "broadcast", broadcast_ ? 1 : 0 );
}

bool SSelectTakeAction::readXml( const QDomElement &elem, int /*version*/ )
{
    clipPath_ = stringToPath( elem.attribute( "clip" ) );
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
