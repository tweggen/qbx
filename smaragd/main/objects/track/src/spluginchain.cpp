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
        // The base implementation is what registers the link in childOrder_, so
        // run it first and then read the index from there — childOrder_ is the
        // authority on slot order (getSlotAt() reads it), not QObject::children().
        SObject::childEvent( event );
        SLink *link = qobject_cast<SLink *>( event->child() );
        if( link ) {
            if( SPluginSlot *slot = dynamic_cast<SPluginSlot *>( &link->getSObject() ) ) {
                const int index = indexOfChild( link );
                if( index >= 0 ) emit slotInserted( index, *slot );
            }
        }
        return;
    }

    if( event->type() == QEvent::ChildRemoved ) {
        // The index MUST be taken BEFORE the base implementation drops the link
        // from childOrder_ — and it cannot come from children(), because Qt has
        // ALREADY removed the child by the time ChildRemoved is delivered.
        //
        // That was a real, silent bug: the old code did
        // `children().indexOf(child)`, which is therefore ALWAYS -1 here, so
        // slotRemoved was NEVER emitted. STrack::onPluginSlotRemoved never ran,
        // the removed slot's per-bus taps stayed wired into every twPluginChain,
        // and remove-plugin changed the model while leaving the audio completely
        // untouched (verified by rendering after a removal: byte-for-byte the
        // processed result). Undo then compounded it.
        //
        // Dereferencing the link here is safe because ~SLink calls
        // setParent(nullptr) while it is still a fully-typed, live SLink — see
        // the comment in slink.cpp.
        SLink *link = static_cast<SLink *>( (QObject *)event->child() );
        const int index = indexOfChild( link );
        SPluginSlot *slot = ( index >= 0 )
            ? dynamic_cast<SPluginSlot *>( &link->getSObject() )
            : nullptr;
        SObject::childEvent( event );
        if( slot ) emit slotRemoved( index, *slot );
        return;
    }

    // Default: call parent for other event types
    SObject::childEvent( event );
}

SPluginChain::~SPluginChain() = default;

// Must NOT throw (proposal 08 M4). It used to, unconditionally: getChainComponent()
// was a TODO stub returning a member nothing ever assigned, so every call — and
// SLink::getRootComponent() reaches this for any link to a chain — was a
// std::runtime_error out of the model layer. A chain that no track has adopted
// yet legitimately has no component; null says so.
std::shared_ptr<twComponent> SPluginChain::getRootComponent()
{
    return getChainComponent();
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

std::shared_ptr<twComponent> SPluginChain::getChainComponent()
{
    // The owning STrack installs the provider (setComponentProvider) and hands
    // out bus 0's twPluginChain. Null before adoption, never a throw.
    return componentProvider_ ? componentProvider_() : nullptr;
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
