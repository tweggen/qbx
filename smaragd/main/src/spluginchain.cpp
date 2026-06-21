#include "spluginchain.h"
#include "spluginslot.h"
#include "sproject.h"
#include "sapplication.h"
#include "twmixer.h"
#include "twcomponent.h"
#include "slink.h"
#include <QDomElement>
#include <QTextStream>
#include <QChildEvent>

SPluginChain::SPluginChain( SProject *project )
    : SObject( project )
{
    setSName( "Effects" );
    // Create a mixer component to thread audio through the chain.
    // With zero children, it's a simple pass-through.
    // As slots are added, they become inputs to the mixer.
}

void SPluginChain::childEvent( QChildEvent *event )
{
    if( event->type() == QEvent::ChildAdded ) {
        // Find the index of the newly added child before parent processes it
        QObject *child = event->child();
        int index = children().indexOf( child );
        if( index >= 0 ) {
            SLink *link = dynamic_cast<SLink *>( child );
            if( link ) {
                SPluginSlot *slot = dynamic_cast<SPluginSlot *>( &link->getSObject() );
                if( slot ) {
                    // Cache the slot before parent's childEvent runs
                    SObject::childEvent( event );
                    emit slotInserted( index, *slot );
                    return;
                }
            }
        }
    } else if( event->type() == QEvent::ChildRemoved ) {
        // Extract child info BEFORE parent's childEvent deletes it
        QObject *child = event->child();
        int index = children().indexOf( child );
        if( index >= 0 ) {
            SLink *link = dynamic_cast<SLink *>( child );
            if( link ) {
                SPluginSlot *slot = dynamic_cast<SPluginSlot *>( &link->getSObject() );
                if( slot ) {
                    // Let parent process removal first
                    SObject::childEvent( event );
                    emit slotRemoved( index, *slot );
                    return;
                }
            }
        }
    }

    // Default: call parent for other event types
    SObject::childEvent( event );
}

SPluginChain::~SPluginChain() = default;

twComponent &SPluginChain::getRootComponent()
{
    twComponent *comp = getChainComponent();
    if( comp ) return *comp;
    // TODO: build chain component from current slots
    throw std::runtime_error( "SPluginChain: no chain component available" );
}

QWidget *SPluginChain::getDetailEditWidget( QWidget *parent )
{
    // TODO: effects editor widget
    return nullptr;
}

QWidget *SPluginChain::getInlineEditWidget( QWidget *parent )
{
    return nullptr;
}

SObjectRenderer *SPluginChain::getInlineRenderer()
{
    return nullptr;
}

SPluginSlot *SPluginChain::getSlotAt( int index ) const
{
    SLink *lk = childAt( index );
    if( !lk ) return nullptr;
    return dynamic_cast<SPluginSlot *>( &lk->getSObject() );
}

void SPluginChain::reorderSlot( int fromIndex, int toIndex )
{
    moveChildToIndex( fromIndex, toIndex );
    // Emit signal to trigger DSP graph rewiring (STrack::onPluginSlotsReordered listens)
    emit slotsReordered();
}

twComponent *SPluginChain::getChainComponent()
{
    // TODO: Build or rebuild the chain component from current slots.
    // For now, return nullptr (will be implemented with proper wiring).
    return chainComponent_;
}
