#include "spluginslot.h"
#include "sproject.h"
#include "sapplication.h"
#include "twplugininsert.h"
#include "plugins/twplugindescriptor.h"
#include <QDomElement>
#include <QTextStream>

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

    // TODO: read state chunk from element
    return 0;
}

int SPluginSlot::serializeSelfAttributes( QTextStream &o )
{
    o << " bypassed='" << (bypass_ ? "true" : "false") << "'";
    o << " format='" << QString::fromStdString( descriptor_.format ) << "'";
    o << " uid='" << QString::fromStdString( descriptor_.uid ) << "'";
    // TODO: write state chunk as base64
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
