#include "spluginchain.h"
#include "spluginslot.h"
#include "sproject.h"
#include "sapplication.h"
#include "twmixer.h"
#include "twcomponent.h"
#include "slink.h"
#include <QDomElement>
#include <QTextStream>

SPluginChain::SPluginChain( SProject *project )
    : SObject( project )
{
    setSName( "Effects" );
    // Create a mixer component to thread audio through the chain.
    // With zero children, it's a simple pass-through.
    // As slots are added, they become inputs to the mixer.
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
    // Rewire the DSP graph (slots are now in a different order).
    // TODO: trigger chain component rebuild
    emit slotsReordered();
}

twComponent *SPluginChain::getChainComponent()
{
    // TODO: Build or rebuild the chain component from current slots.
    // For now, return nullptr (will be implemented with proper wiring).
    return chainComponent_;
}
