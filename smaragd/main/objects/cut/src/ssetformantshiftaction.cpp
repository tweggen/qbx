#include "app/objects/cut/ssetformantshiftaction.h"
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
// Same take-resolution rule as set-pitch / set-formant-preserve: the
// placement itself, or the ONE take being edited on a stack (reported so the
// inverse can name it).
SCut *formantShiftTargetCut( SObject *root, const QList<int> &clipPath,
                             int take, int &resolvedTake )
{
    resolvedTake = -1;
    SLink *link = splacements::placementAt( root, clipPath );
    if( !link ) return nullptr;

    if( STakeStack *stack = dynamic_cast<STakeStack*>( &link->getSObject() ) ) {
        resolvedTake = ( take >= 0 ) ? take : stack->activeTakeIndex();
        // Formant shift is per-take and audio-only (see set-pitch).
        return dynamic_cast<SCut*>( stack->takeObjectAt( resolvedTake ) );
    }
    return dynamic_cast<SCut*>( &link->getSObject() );
}
}

SSetFormantShiftAction::SSetFormantShiftAction(
        const QList<int> &clipPath, double cents, int take, bool broadcast )
    : clipPath_( clipPath ), cents_( cents ), take_( take ),
      broadcast_( broadcast )
{
}

SApplyResult SSetFormantShiftAction::apply( SProject *project )
{
    if( !project || clipPath_.isEmpty() ) {
        return {false, nullptr};
    }
    SObject *mixer = splacements::rootContainer( project );
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
                formantShiftTargetCut( mixer, clipPath_, -1, resolved );
                t = resolved;
            }
            SCompositeAction composite;
            for( const QList<int> &p : targets ) {
                int resolved = -1;
                if( !formantShiftTargetCut( mixer, p, t, resolved ) ) continue;
                composite.append(
                    new SSetFormantShiftAction( p, cents_, t, false ) );
            }
            if( composite.count() > 0 )
                return composite.apply( project );
            return {false, nullptr};
        }
    }

    int resolvedTake = -1;
    SCut *cut = formantShiftTargetCut( mixer, clipPath_, take_, resolvedTake );
    if( !cut ) {
        return {false, nullptr};
    }

    const double oldCents = cut->getFormantShiftCents();
    cut->setFormantShiftCents( cents_ );   // clamps to SCut::FORMANT_SHIFT_CENTS_LIMIT

    SAction *inverse = new SSetFormantShiftAction( clipPath_, oldCents,
                                                    resolvedTake, false );
    return {true, inverse};
}

void SSetFormantShiftAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", pathToString( clipPath_ ) );
    elem.setAttribute( "cents", QString::number( cents_ ) );
    elem.setAttribute( "take", take_ );
    elem.setAttribute( "broadcast", broadcast_ ? 1 : 0 );
}

bool SSetFormantShiftAction::readXml( const QDomElement &elem, int )
{
    clipPath_  = stringToPath( elem.attribute( "clip" ) );
    cents_     = elem.attribute( "cents", "0" ).toDouble();
    take_      = elem.attribute( "take", "-1" ).toInt();
    broadcast_ = elem.attribute( "broadcast", "1" ).toInt() != 0;
    return true;
}

static const bool s_reg_setformantshift = (
    SActionRegistry::instance().registerType(
        QStringLiteral("set-formant-shift"),
        []{ return new SSetFormantShiftAction; }
    ), true
);
