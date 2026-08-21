#include "app/objects/cut/ssetclipeventchannelaction.h"
#include "app/objects/cut/scut.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/model/slink.h"
#include "app/actions/sactionregistry.h"
#include <QDomElement>
#include <limits>

using namespace strackpath;

SSetClipEventChannelAction::SSetClipEventChannelAction(
    const QList<int> &clipPath, int channel )
    : clipPath_( clipPath ), channel_( channel )
{
}

SApplyResult SSetClipEventChannelAction::apply( SProject *project )
{
    if( !project || clipPath_.isEmpty() ) {
        return {false, nullptr};
    }
    SObject *mixer = splacements::rootNamed( project, pathRoot_ );
    if( !mixer ) {
        return {false, nullptr};
    }
    SLink *link = splacements::placementAt( mixer, clipPath_ );
    if( !link ) {
        return {false, nullptr};
    }
    // Scoped to a windowed (SCut) placement, matching the fragment/asset
    // shape this verb exists for; the remap itself is stored on the SLink
    // (proposal 41 D6: per-PLACEMENT, since D2 shares one SCut across every
    // placement of an asset and a remap on the content would move them all).
    if( !dynamic_cast<SCut*>( &link->getSObject() ) ) {
        return {false, nullptr};
    }

    const int oldChannel = link->getEventChannelOverride();
    link->setEventChannelOverride( channel_ );   // clamps to -1 / 0..15
    // Open-ended: the consumer of an event stream is class-1 (design F9),
    // so a channel change is never bounded on the right.
    link->getSObject().invalidateRenderPathRange(
        0, std::numeric_limits<offset_t>::max() );

    SAction *inverse = new SSetClipEventChannelAction( clipPath_, oldChannel );
    return {true, inverse};
}

void SSetClipEventChannelAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", qualifiedToString( pathRoot_, clipPath_ ) );
    elem.setAttribute( "channel", channel_ );
}

bool SSetClipEventChannelAction::readXml( const QDomElement &elem, int /*version*/ )
{
    clipPath_ = parseInto( pathRoot_, elem.attribute( "clip" ) );
    channel_  = elem.attribute( "channel", "-1" ).toInt();
    return true;
}

static const bool s_reg_setclipeventchannel = (
    SActionRegistry::instance().registerType(
        QStringLiteral("set-clip-event-channel"),
        []{ return new SSetClipEventChannelAction; }
    ), true
);
