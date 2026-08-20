#include "app/objects/cut/ssetclipvolumeaction.h"
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
#include <cmath>

using namespace strackpath;

namespace {
// Same take-resolution rule as set-pitch / set-formant-preserve: the
// placement itself, or the ONE take being edited on a stack (reported so the
// inverse can name it instead of re-resolving against a possibly different
// active take). Volume is audio-only, same as formant preservation.
SCut *volumeTargetCut( SObject *root, const QList<int> &clipPath, int take,
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

SSetClipVolumeAction::SSetClipVolumeAction(
        const QList<int> &clipPath, double volumeDb, int take, bool broadcast )
    : clipPath_( clipPath ), volumeDb_( clampVolumeDb( volumeDb ) ),
      take_( take ), broadcast_( broadcast )
{
}

SApplyResult SSetClipVolumeAction::apply( SProject *project )
{
    if( !project || clipPath_.isEmpty() ) {
        return {false, nullptr};
    }
    SObject *mixer = splacements::rootNamed( project, pathRoot_ );
    if( !mixer ) {
        return {false, nullptr};
    }

    // Edit-group broadcast, mirroring set-pitch / set-formant-preserve.
    if( broadcast_ ) {
        QList<QList<int>> targets =
            seditgroups::expandClipPaths( project, clipPath_ );
        if( targets.size() > 1 ) {
            int t = take_;
            if( t < 0 ) {
                int resolved = -1;
                volumeTargetCut( mixer, clipPath_, -1, resolved );
                t = resolved;
            }
            SCompositeAction composite;
            for( const QList<int> &p : targets ) {
                int resolved = -1;
                if( !volumeTargetCut( mixer, p, t, resolved ) ) continue;
                composite.append(
                    new SSetClipVolumeAction( p, volumeDb_, t, false ) );
            }
            if( composite.count() > 0 )
                return composite.apply( project );
            return {false, nullptr};
        }
    }

    int resolvedTake = -1;
    SCut *cut = volumeTargetCut( mixer, clipPath_, take_, resolvedTake );
    if( !cut ) {
        return {false, nullptr};
    }

    const double oldDb = cut->getVolume();
    cut->setVolume( volumeDb_ );

    SAction *inverse = new SSetClipVolumeAction( clipPath_, oldDb,
                                                 resolvedTake, false );
    return {true, inverse};
}

void SSetClipVolumeAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", qualifiedToString( pathRoot_, clipPath_ ) );
    elem.setAttribute( "volumeDb", QString::number( volumeDb_ ) );
    elem.setAttribute( "take", take_ );
    elem.setAttribute( "broadcast", broadcast_ ? 1 : 0 );
}

bool SSetClipVolumeAction::readXml( const QDomElement &elem, int )
{
    clipPath_  = parseInto( pathRoot_, elem.attribute( "clip" ) );
    volumeDb_  = clampVolumeDb( elem.attribute( "volumeDb", "0" ).toDouble() );
    take_      = elem.attribute( "take", "-1" ).toInt();
    broadcast_ = elem.attribute( "broadcast", "1" ).toInt() != 0;
    return true;
}

static const bool s_reg_setclipvolume = (
    SActionRegistry::instance().registerType(
        QStringLiteral("set-clip-volume"),
        []{ return new SSetClipVolumeAction; }
    ), true
);
