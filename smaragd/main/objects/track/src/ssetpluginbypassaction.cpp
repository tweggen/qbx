#include "app/objects/track/ssetpluginbypassaction.h"

#include "app/actions/sactionregistry.h"
#include "app/objects/track/spluginactionsupport.h"

#include <QDebug>
#include <QDomElement>

SSetPluginBypassAction::SSetPluginBypassAction( const QString &trackPath,
                                                int slotIndex, bool bypassed )
    : trackPath_( trackPath ), slotIndex_( slotIndex ), bypassed_( bypassed )
{
}

SApplyResult SSetPluginBypassAction::apply( SProject *project )
{
    SPluginSlot *slot = spluginaction::slotFor( project, trackPath_, slotIndex_ );
    if( !slot ) {
        qWarning() << "set-plugin-bypass: no slot" << slotIndex_
                   << "on track" << trackPath_;
        return { false, nullptr };
    }

    const bool oldBypass = slot->getBypass();

    // Everything audible about a bypass toggle is inside setBypass(): it
    // forwards to the SHARED processor (one call reaches every bus), bumps the
    // param epoch so the taps' cached pages are staled, and invalidates the
    // render path above the slot. Without those the model would flip and the
    // render would be byte-identical.
    slot->setBypass( bypassed_ );

    return { true, new SSetPluginBypassAction( trackPath_, slotIndex_, oldBypass ) };
}

void SSetPluginBypassAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "slotIndex", slotIndex_ );
    elem.setAttribute( "bypassed", bypassed_ ? "true" : "false" );
}

bool SSetPluginBypassAction::readXml( const QDomElement &elem, int /*version*/ )
{
    trackPath_ = elem.attribute( "trackPath" );
    slotIndex_ = elem.attribute( "slotIndex", "0" ).toInt();
    bypassed_  = elem.attribute( "bypassed", "false" ) == "true";
    return true;
}

static const bool s_reg_setpluginbypass =
    ( SActionRegistry::instance().registerType(
          QStringLiteral( "set-plugin-bypass" ),
          [] { return new SSetPluginBypassAction; } ),
      true );
