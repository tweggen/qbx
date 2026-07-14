#include "app/objects/track/spluginchain.h"
#include "app/objects/track/spluginslot.h"
#include "app/model/sproject.h"
#include "tw/mix/twmixer.h"
#include "tw/graph/twcomponent.h"
#include "app/model/slink.h"
#include "app/persistence/sprojectloader.h"
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
        // Do NOT use children().indexOf() here: by the time a ChildRemoved event
        // is delivered, Qt has already dropped the child from children() (that
        // removal is what fires the event — see SLink::~SLink's setParent(nullptr)),
        // so indexOf() would return -1 and slotRemoved would never be emitted,
        // leaving the DSP chain holding a dangling twPluginInsert*. Instead read
        // the logical index from childOrder_, which still contains the link until
        // the base childEvent runs below. The link and its slot are still fully
        // alive at this point (removeRef runs after setParent(nullptr)).
        QObject *child = event->child();
        SLink *link = dynamic_cast<SLink *>( child );
        if( link ) {
            int index = indexOfChild( link );
            SPluginSlot *slot = dynamic_cast<SPluginSlot *>( &link->getSObject() );
            if( index >= 0 && slot ) {
                // Let parent process removal first (drops link from childOrder_)
                SObject::childEvent( event );
                emit slotRemoved( index, *slot );
                return;
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
    // Emit signal to trigger DSP graph reordering (STrack::onPluginSlotsReordered
    // listens). Pass from/to so the DSP chain reorders its plugin vector to match
    // the new model order — rewiring alone would leave plugins_ in the old order.
    emit slotsReordered( fromIndex, toIndex );
}

twComponent *SPluginChain::getChainComponent()
{
    // TODO: Build or rebuild the chain component from current slots.
    // For now, return nullptr (will be implemented with proper wiring).
    return chainComponent_;
}

SLink *SPluginChain::instantiateFromDomElement(
    SProjectLoader &projectLoader, QDomElement &element, SObject *parent )
{
    (void) parent;
    // Create the plugin chain object
    SPluginChain *chain = new SPluginChain( &projectLoader.getProject() );
    chain->readPreChildrenAttributes( element );

    // Iterate through child SLink elements and instantiate plugin slots
    QDomNode childNode = element.firstChild();
    while( !childNode.isNull() ) {
        if( childNode.isElement() ) {
            QDomElement childElement = childNode.toElement();
            if( childElement.nodeName() == "SLink" ) {
                QString objectId = childElement.attribute( "objectId" );
                // Look up the object id in the dictionary
                SLink *contentLink = projectLoader.getObjectDictionary().value( objectId );
                if( contentLink ) {
                    // Create a link to the slot
                    SLink *sl = new SLink( contentLink->getSObject(), nullptr );
                    if( sl ) {
                        sl->setParent( chain );
                    }
                }
            }
        }
        childNode = childNode.nextSibling();
    }

    chain->readPostChildrenAttributes( element );
    return new SLink( *chain, nullptr );
}

// Self-registration with the project loader (proposal 14, Phase 5): the
// persistence module names no concrete types; each slice registers its own
// element name. Relies on the app being an OBJECT library (no TU elision).
static const bool s_registered_spluginchain =
    ( SProjectLoader::registerSObjectClass( "SPluginChain",
          SPluginChain::instantiateFromDomElement ), true );
