#include "app/objects/cut/ssetformantpreserveaction.h"
#include "app/objects/cut/scut.h"
#include "app/objects/cut/stakestack.h"
#include "app/model/seditgroups.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/model/slink.h"
#include "app/actions/sactionregistry.h"
#include "app/actions/scompositeaction.h"
#include <QDomElement>

using namespace strackpath;

namespace {
// Same take-resolution rule as set-pitch: the placement itself, or the ONE
// take being edited on a stack (reported so the inverse can name it).
SCut *formantTargetCut( SObject *root, const QList<int> &clipPath, int take,
                        int &resolvedTake )
{
    resolvedTake = -1;
    SLink *link = splacements::placementAt( root, clipPath );
    if( !link ) return nullptr;

    if( STakeStack *stack = dynamic_cast<STakeStack*>( &link->getSObject() ) ) {
        resolvedTake = ( take >= 0 ) ? take : stack->activeTakeIndex();
        // Formant preservation is per-take and audio-only (see set-pitch).
        return dynamic_cast<SCut*>( stack->takeObjectAt( resolvedTake ) );
    }
    return dynamic_cast<SCut*>( &link->getSObject() );
}
}

SSetFormantPreserveAction::SSetFormantPreserveAction(
        const QList<int> &clipPath, bool on, int take, bool broadcast )
    : clipPath_( clipPath ), on_( on ), take_( take ), broadcast_( broadcast )
{
}

SApplyResult SSetFormantPreserveAction::apply( SProject *project )
{
    if( !project || clipPath_.isEmpty() ) {
        return {false, nullptr};
    }
    SObject *mixer = splacements::rootContainer( project );
    if( !mixer ) {
        return {false, nullptr};
    }

    // Edit-group broadcast, mirroring set-pitch.
    if( broadcast_ ) {
        QList<QList<int>> targets =
            seditgroups::expandClipPaths( project, clipPath_ );
        if( targets.size() > 1 ) {
            int t = take_;
            if( t < 0 ) {
                int resolved = -1;
                formantTargetCut( mixer, clipPath_, -1, resolved );
                t = resolved;
            }
            SCompositeAction composite;
            for( const QList<int> &p : targets ) {
                int resolved = -1;
                if( !formantTargetCut( mixer, p, t, resolved ) ) continue;
                composite.append(
                    new SSetFormantPreserveAction( p, on_, t, false ) );
            }
            if( composite.count() > 0 )
                return composite.apply( project );
            return {false, nullptr};
        }
    }

    int resolvedTake = -1;
    SCut *cut = formantTargetCut( mixer, clipPath_, take_, resolvedTake );
    if( !cut ) {
        return {false, nullptr};
    }

    const bool oldOn = cut->getPreserveFormants();
    cut->setPreserveFormants( on_ );

    SAction *inverse = new SSetFormantPreserveAction( clipPath_, oldOn,
                                                      resolvedTake, false );
    return {true, inverse};
}

void SSetFormantPreserveAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", pathToString( clipPath_ ) );
    elem.setAttribute( "on", on_ ? 1 : 0 );
    elem.setAttribute( "take", take_ );
    elem.setAttribute( "broadcast", broadcast_ ? 1 : 0 );
}

bool SSetFormantPreserveAction::readXml( const QDomElement &elem, int )
{
    clipPath_  = stringToPath( elem.attribute( "clip" ) );
    on_        = elem.attribute( "on", "0" ).toInt() != 0;
    take_      = elem.attribute( "take", "-1" ).toInt();
    broadcast_ = elem.attribute( "broadcast", "1" ).toInt() != 0;
    return true;
}

static const bool s_reg_setformantpreserve = (
    SActionRegistry::instance().registerType(
        QStringLiteral("set-formant-preserve"),
        []{ return new SSetFormantPreserveAction; }
    ), true
);
