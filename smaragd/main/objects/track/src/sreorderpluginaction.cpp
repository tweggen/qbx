#include "app/objects/track/sreorderpluginaction.h"

#include "app/actions/sactionregistry.h"
#include "app/objects/track/spluginactionsupport.h"

#include <QDebug>
#include <QDomElement>

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
