#include "app/objects/cut/ssetclippanaction.h"
#include "app/objects/cut/scut.h"
#include "app/objects/cut/stakestack.h"
#include "app/model/seditgroups.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/model/slink.h"
#include "app/actions/sactionregistry.h"
#include "app/actions/scompositeaction.h"
#include "tw/core/twlog.h"
#include <QDomElement>

using namespace strackpath;

namespace {
// Same take-resolution rule as set-clip-volume / set-pitch: the placement
// itself, or the ONE take being edited on a stack.
SCut *panTargetCut( SObject *root, const QList<int> &clipPath, int take,
                    int &resolvedTake )
{
    resolvedTake = -1;
    SLink *link = splacements::placementAt( root, clipPath );
    if( !link ) return nullptr;

    if( STakeStack *stack = dynamic_cast<STakeStack*>( &link->getSObject() ) ) {
        resolvedTake = ( take >= 0 ) ? take : stack->activeTakeIndex();
        return dynamic_cast<SCut*>( stack->takeObjectAt( resolvedTake ) );
    }
    return dynamic_cast<SCut*>( &link->getSObject() );
}
}

SSetClipPanAction::SSetClipPanAction(
        const QList<int> &clipPath, double pan, int take, bool broadcast )
    : clipPath_( clipPath ), pan_( pan ), take_( take ), broadcast_( broadcast )
{
}

SApplyResult SSetClipPanAction::apply( SProject *project )
{
    if( !project || clipPath_.isEmpty() ) {
        return {false, nullptr};
    }
    SObject *mixer = splacements::rootNamed( project, pathRoot_ );
    if( !mixer ) {
        return {false, nullptr};
    }

    // PAN IS DEFINED AT WIDTH 2 ONLY (proposal 49 D1). At any other width the
    // mix passes a clip through as if centred, so a non-zero value would be a
    // control that does nothing -- refused, and ANNOUNCED. 0 is always
    // accepted, so a reset can never fail. Checked before the broadcast, so
    // a refused edit touches no group member either.
    if( pan_ != 0.0 && project->channels() != 2 ) {
        // QString::number, not %f: this box's LC_NUMERIC is de_DE, and a
        // printf-spelled "0,5000" would read as a different number in the log.
        TW_LOGW( "cut", "set-clip-pan: refusing pan %s on a %d-channel "
                        "project; pan is defined for 2 channels only",
                 qPrintable( QString::number( pan_ ) ), project->channels() );
        return {false, nullptr};
    }

    // Edit-group broadcast, mirroring set-clip-volume / set-pitch.
    if( broadcast_ ) {
        QList<QList<int>> targets =
            seditgroups::expandClipPaths( project, clipPath_ );
        if( targets.size() > 1 ) {
            int t = take_;
            if( t < 0 ) {
                int resolved = -1;
                panTargetCut( mixer, clipPath_, -1, resolved );
                t = resolved;
            }
            SCompositeAction composite;
            for( const QList<int> &p : targets ) {
                int resolved = -1;
                if( !panTargetCut( mixer, p, t, resolved ) ) continue;
                composite.append(
                    new SSetClipPanAction( p, pan_, t, false ) );
            }
            if( composite.count() > 0 )
                return composite.apply( project );
            return {false, nullptr};
        }
    }

    int resolvedTake = -1;
    SCut *cut = panTargetCut( mixer, clipPath_, take_, resolvedTake );
    if( !cut ) {
        return {false, nullptr};
    }

    const double oldPan = cut->getPan();
    cut->setPan( pan_ );   // clamps to [-1, 1] itself (SObject::setPan)

    SAction *inverse = new SSetClipPanAction( clipPath_, oldPan,
                                              resolvedTake, false );
    return {true, inverse};
}

void SSetClipPanAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", qualifiedToString( pathRoot_, clipPath_ ) );
    elem.setAttribute( "pan", QString::number( pan_ ) );
    elem.setAttribute( "take", take_ );
    elem.setAttribute( "broadcast", broadcast_ ? 1 : 0 );
}

bool SSetClipPanAction::readXml( const QDomElement &elem, int )
{
    clipPath_  = parseInto( pathRoot_, elem.attribute( "clip" ) );
    pan_       = elem.attribute( "pan", "0" ).toDouble();
    take_      = elem.attribute( "take", "-1" ).toInt();
    broadcast_ = elem.attribute( "broadcast", "1" ).toInt() != 0;
    return true;
}

static const bool s_reg_setclippan = (
    SActionRegistry::instance().registerType(
        QStringLiteral("set-clip-pan"),
        []{ return new SSetClipPanAction; }
    ), true
);
