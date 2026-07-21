#include "app/objects/cut/ssetpitchaction.h"
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
// Resolve the SCut a clip path addresses: the placement itself, or — for a
// take stack — the ONE take being transposed. Reports which take that was, so
// the inverse can name it explicitly (the active take may change before undo).
SCut *pitchTargetCut( SObject *root, const QList<int> &clipPath, int take,
                      int &resolvedTake )
{
    resolvedTake = -1;
    SLink *link = splacements::placementAt( root, clipPath );
    if( !link ) return nullptr;

    if( STakeStack *stack = dynamic_cast<STakeStack*>( &link->getSObject() ) ) {
        resolvedTake = ( take >= 0 ) ? take : stack->activeTakeIndex();
        return stack->takeCutAt( resolvedTake );   // null when there is no such take
    }
    return dynamic_cast<SCut*>( &link->getSObject() );
}
}

SSetPitchAction::SSetPitchAction( const QList<int> &clipPath, double cents,
                                  int take, bool broadcast )
    : clipPath_( clipPath ), cents_( cents ), take_( take ),
      broadcast_( broadcast )
{
}

SApplyResult SSetPitchAction::apply( SProject *project )
{
    if( !project || clipPath_.isEmpty() ) {
        return {false, nullptr};
    }
    SObject *mixer = splacements::rootContainer( project );
    if( !mixer ) {
        return {false, nullptr};
    }

    // Edit-group broadcast: the same transposition on every member's
    // corresponding clip. Like select-take, members whose clip cannot be
    // transposed are skipped up front rather than failing the whole composite.
    if( broadcast_ ) {
        QList<QList<int>> targets =
            seditgroups::expandClipPaths( project, clipPath_ );
        if( targets.size() > 1 ) {
            // An active-take anchor resolves to an explicit index before
            // fanning out, so every member transposes the SAME lane.
            int t = take_;
            if( t < 0 ) {
                int resolved = -1;
                pitchTargetCut( mixer, clipPath_, -1, resolved );
                t = resolved;
            }
            SCompositeAction composite;
            for( const QList<int> &p : targets ) {
                int resolved = -1;
                if( !pitchTargetCut( mixer, p, t, resolved ) ) continue;
                composite.append( new SSetPitchAction( p, cents_, t, false ) );
            }
            if( composite.count() > 0 )
                return composite.apply( project );
            return {false, nullptr};
        }
    }

    int resolvedTake = -1;
    SCut *cut = pitchTargetCut( mixer, clipPath_, take_, resolvedTake );
    if( !cut ) {
        return {false, nullptr};   // not a clip we can transpose
    }

    const double oldCents = cut->getPitchCents();
    cut->setPitchCents( cents_ );   // clamps to SCut::PITCH_CENTS_LIMIT

    // The inverse names the take EXPLICITLY: undo must transpose the lane this
    // edit touched, even if the active take changed in between.
    SAction *inverse = new SSetPitchAction( clipPath_, oldCents,
                                            resolvedTake, false );
    return {true, inverse};
}

void SSetPitchAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", pathToString( clipPath_ ) );
    elem.setAttribute( "cents", QString::number( cents_ ) );
    elem.setAttribute( "take", take_ );
    elem.setAttribute( "broadcast", broadcast_ ? 1 : 0 );
}

bool SSetPitchAction::readXml( const QDomElement &elem, int /*version*/ )
{
    clipPath_  = stringToPath( elem.attribute( "clip" ) );
    cents_     = elem.attribute( "cents", "0" ).toDouble();
    take_      = elem.attribute( "take", "-1" ).toInt();
    broadcast_ = elem.attribute( "broadcast", "1" ).toInt() != 0;
    return true;
}

static const bool s_reg_setpitch = (
    SActionRegistry::instance().registerType(
        QStringLiteral("set-pitch"),
        []{ return new SSetPitchAction; }
    ), true
);
