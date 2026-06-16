#include "spluginslot.h"
#include "sproject.h"
#include "sapplication.h"
#include "twplugininsert.h"
#include "plugins/twplugindescriptor.h"
#include <QDomElement>
#include <QTextStream>
#include <QByteArray>

SPluginSlot::SPluginSlot( SProject *project, const audio::twPluginDescriptor &desc )
    : SObject( project ), descriptor_( desc )
{
    setSName( QString::fromStdString( desc.name ) );

    // Instantiate the plugin via the registry.
    auto &registry = audio::pluginRegistry();
    auto plugin = registry.instantiate( desc );
    if( !plugin ) {
        qWarning( "SPluginSlot: failed to instantiate plugin '%s'", desc.name.c_str() );
        return;
    }

    // Create the host component (twPluginInsert).
    insert_ = std::make_unique<audio::twPluginInsert>(
        *SApplication::app().get303aEnvironment(), std::move( plugin ) );
}

SPluginSlot::~SPluginSlot() = default;

twComponent &SPluginSlot::getRootComponent()
{
    if( insert_ ) {
        return *insert_;
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
        if( insert_ && insert_->getPlugin() ) {
            insert_->getPlugin()->loadState( savedState_ );
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
        if( insert_ )
            insert_->setBypass( bypass );
        emit bypassChanged( bypass );
    }
}

void SPluginSlot::saveState( std::vector<std::uint8_t> &state )
{
    if( insert_ && insert_->getPlugin() ) {
        state = insert_->getPlugin()->saveState();
    }
}

void SPluginSlot::restoreState( const std::vector<std::uint8_t> &state )
{
    if( insert_ && insert_->getPlugin() ) {
        insert_->getPlugin()->loadState( state );
    }
}

void SPluginSlot::serializeStateChunk( QDomElement &parentElem, QDomDocument &doc )
{
    // Save current state from the plugin
    if( insert_ && insert_->getPlugin() ) {
        savedState_ = insert_->getPlugin()->saveState();
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
