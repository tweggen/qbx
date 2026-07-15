#include "app/objects/track/spluginslot.h"
#include "app/model/sproject.h"
#include "app/model/sappcontext.h"
#include "tw/plugins/twplugininsert.h"
#include "tw/plugins/twplugindescriptor.h"
#include <QDomElement>
#include <QTextStream>
#include <QByteArray>

SPluginSlot::SPluginSlot( SProject *project, const audio::twPluginDescriptor &desc )
    : SObject( project ), descriptor_( desc )
{
    setSName( QString::fromStdString( desc.name ) );
    // Inserts are created on-demand via getInsertForBus()
}

SPluginSlot::~SPluginSlot() = default;

std::shared_ptr<audio::twPluginInsert> SPluginSlot::getInsertForBus( int busIndex ) const
{
    // Create inserts on-demand up to the requested bus index
    if( busIndex < 0 ) return nullptr;

    SPluginSlot *self = const_cast<SPluginSlot *>(this);
    while( (int)self->inserts_.size() <= busIndex ) {
        auto &registry = audio::pluginRegistry();
        auto plugin = registry.instantiate( descriptor_ );
        if( !plugin ) {
            qWarning( "SPluginSlot: failed to instantiate plugin '%s'", descriptor_.name.c_str() );
            return nullptr;
        }

        auto insert = std::make_shared<audio::twPluginInsert>(
            *SAppContext::get().get303aEnvironment(), std::move( plugin ) );

        // Initialize the insert component (allocates plugs and latches)
        insert->init();

        // Verify the insert was properly initialized (has input/output plugs)
        if( insert->getNInputs() == 0 ) {
            qWarning( "SPluginSlot: plugin has 0 inputs, this will crash during rendering" );
            return nullptr;
        }

        self->inserts_.push_back( std::move( insert ) );
    }

    return self->inserts_[busIndex];
}

std::shared_ptr<twComponent> SPluginSlot::getRootComponent()
{
    auto insert = getInsertForBus(0);
    if( insert ) {
        return std::static_pointer_cast<twComponent>(insert);
    }
    // Fallback (shouldn't happen - insert should always exist).
    throw std::runtime_error( "SPluginSlot: no plugin insert available" );
}

QWidget *SPluginSlot::getDetailEditWidget( QWidget *parent )
{
    // TODO: parameter editor widget
    return nullptr;
}

QWidget *SPluginSlot::getInlineEditWidget( QWidget *parent )
{
    return nullptr;
}

SObjectRenderer *SPluginSlot::getInlineRenderer()
{
    return nullptr;
}

int SPluginSlot::readPreChildrenAttributes( QDomElement &element )
{
    if( element.hasAttribute( "bypassed" ) )
        bypass_ = element.attribute( "bypassed" ) == "true";

    // Read the opaque state chunk (base64 encoded in child <state> element)
    QDomElement stateElem = element.firstChildElement( "state" );
    if( !stateElem.isNull() ) {
        QString encoded = stateElem.text();
        QByteArray decoded = QByteArray::fromBase64( encoded.toLatin1() );
        savedState_.resize( decoded.size() );
        std::copy( decoded.begin(), decoded.end(), savedState_.begin() );

        // Restore state to the plugin
        auto insert = getInsertForBus(0);
        if( insert && insert->getPlugin() ) {
            insert->getPlugin()->loadState( savedState_ );
        }
    }

    return 0;
}

int SPluginSlot::serializeSelfAttributes( QTextStream &o )
{
    o << " bypassed='" << (bypass_ ? "true" : "false") << "'";
    o << " format='" << QString::fromStdString( descriptor_.format ) << "'";
    o << " uid='" << QString::fromStdString( descriptor_.uid ) << "'";
    o << " vendor='" << QString::fromStdString( descriptor_.vendor ) << "'";
    o << " nIn='" << descriptor_.io.audioInputs << "'";
    o << " nOut='" << descriptor_.io.audioOutputs << "'";
    return 0;
}

void SPluginSlot::setBypass( bool bypass )
{
    if( bypass_ != bypass ) {
        bypass_ = bypass;
        // Apply bypass to all created inserts
        for( auto &insert : inserts_ ) {
            if( insert )
                insert->setBypass( bypass );
        }
        emit bypassChanged( bypass );
    }
}

void SPluginSlot::saveState( std::vector<std::uint8_t> &state )
{
    auto insert = getInsertForBus(0);
    if( insert && insert->getPlugin() ) {
        state = insert->getPlugin()->saveState();
    }
}

void SPluginSlot::restoreState( const std::vector<std::uint8_t> &state )
{
    auto insert = getInsertForBus(0);
    if( insert && insert->getPlugin() ) {
        insert->getPlugin()->loadState( state );
    }
    // Apply state to all created inserts
    for( auto &ins : inserts_ ) {
        if( ins && ins->getPlugin() ) {
            ins->getPlugin()->loadState( state );
        }
    }
}

void SPluginSlot::serializeStateChunk( QDomElement &parentElem, QDomDocument &doc )
{
    // Save current state from the plugin
    auto insert = getInsertForBus(0);
    if( insert && insert->getPlugin() ) {
        savedState_ = insert->getPlugin()->saveState();
    }

    if( !savedState_.empty() ) {
        QDomElement stateElem = doc.createElement( "state" );
        stateElem.setAttribute( "encoding", "base64" );

        QByteArray data( (const char*)savedState_.data(), (int)savedState_.size() );
        QString encoded = QString::fromLatin1( data.toBase64() );
        stateElem.appendChild( doc.createTextNode( encoded ) );

        parentElem.appendChild( stateElem );
    }
}
