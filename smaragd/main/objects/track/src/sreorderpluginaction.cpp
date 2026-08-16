#include "app/objects/track/sreorderpluginaction.h"

#include "app/actions/sactionregistry.h"
#include "app/objects/track/spluginactionsupport.h"

#include "app/model/splacements.h"
#include "app/objects/track/spluginchain.h"
#include "app/objects/track/spluginslot.h"
#include "app/objects/track/strack.h"
#include "app/objects/track/strackpath.h"

#include <QDebug>
#include <QDomElement>

namespace {

// The instrument, resolved from the TRACK rather than from the chain: "slot 0
// carries isInstrument" is STrack::instrumentSlot()'s definition and there must
// not be a second spelling of it.
bool instrumentAt0( SProject *project, const QString &trackPath )
{
    SObject *root = splacements::rootContainer( project );
    SObject *lane = splacements::laneAt( root, strackpath::stringToPath( trackPath ) );
    STrack  *track = dynamic_cast<STrack *>( lane );
    return track && track->instrumentSlot() != nullptr;
}

}  // namespace

SReorderPluginAction::SReorderPluginAction( const QString &trackPath,
                                           int fromIndex, int toIndex )
    : trackPath_( trackPath ), fromIndex_( fromIndex ), toIndex_( toIndex )
{
}

SApplyResult SReorderPluginAction::apply( SProject *project )
{
    SPluginChain *chain = spluginaction::chainFor( project, trackPath_ );
    if( !chain ) {
        qWarning() << "reorder-plugin: no plugin chain on track" << trackPath_;
        return { false, nullptr };
    }

    const int n = chain->getSlotCount();
    // Validate BOTH ends against the real chain. moveChildToIndex() silently
    // clamps and silently returns on an out-of-range source, so an unchecked
    // action would report success while having moved nothing — and its inverse
    // would then move something that never moved.
    if( fromIndex_ < 0 || fromIndex_ >= n || toIndex_ < 0 || toIndex_ >= n ) {
        qWarning() << "reorder-plugin: index out of range" << fromIndex_ << "->"
                   << toIndex_ << "of" << n << "slots on track" << trackPath_;
        return { false, nullptr };
    }
    // AN EFFECT CANNOT MOVE IN FRONT OF THE INSTRUMENT, and the instrument
    // cannot move out of slot 0 (design D3). Both are the same refusal: the
    // instrument's role and its position are one fact, and a chain whose slot 0
    // is an effect fed by nothing would simply be silent.
    if( ( fromIndex_ == 0 || toIndex_ == 0 ) && instrumentAt0( project, trackPath_ ) ) {
        qWarning() << "reorder-plugin: slot 0 of track" << trackPath_
                   << "is an instrument; it cannot be moved and nothing may move"
                      " in front of it";
        return { false, nullptr };
    }

    if( fromIndex_ == toIndex_ ) {
        // A no-op is not a failure, but it must not enter the undo stack with a
        // pretend inverse either.
        return { true, nullptr };
    }

    // reorderSlot() moves the link in childOrder_ AND emits slotsReordered(),
    // which is what makes STrack rebuild every bus's twPluginChain wiring and
    // invalidate the render path. Reordering the model alone would be inaudible.
    chain->reorderSlot( fromIndex_, toIndex_ );

    return { true, new SReorderPluginAction( trackPath_, toIndex_, fromIndex_ ) };
}

void SReorderPluginAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "fromIndex", fromIndex_ );
    elem.setAttribute( "toIndex", toIndex_ );
}

bool SReorderPluginAction::readXml( const QDomElement &elem, int /*version*/ )
{
    trackPath_ = elem.attribute( "trackPath" );
    fromIndex_ = elem.attribute( "fromIndex", "0" ).toInt();
    toIndex_   = elem.attribute( "toIndex", "0" ).toInt();
    return true;
}

static const bool s_reg_reorderplugin =
    ( SActionRegistry::instance().registerType(
          QStringLiteral( "reorder-plugin" ),
          [] { return new SReorderPluginAction; } ),
      true );
