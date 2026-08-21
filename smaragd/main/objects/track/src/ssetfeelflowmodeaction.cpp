#include "app/objects/track/ssetfeelflowmodeaction.h"

#include "app/actions/sactionregistry.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/objects/track/strackpath.h"

#include <QDebug>
#include <QDomElement>

using namespace strackpath;

SApplyResult SSetFeelFlowModeAction::apply( SProject *project )
{
    SObject *root = splacements::rootContainer( project );
    SObject *lane = splacements::laneAt( root, trackPath_ );
    STrack *track = dynamic_cast<STrack *>( lane );
    if( !track ) {
        qWarning() << "set-feel-flow-mode: no track at path" << pathToString( trackPath_ );
        return { false, nullptr };
    }

    const STrack::FeelFlowMode oldMode = track->feelFlowMode();
    track->setFeelFlowModeInternal( mode_ );

    return { true, new SSetFeelFlowModeAction( trackPath_, oldMode ) };
}

void SSetFeelFlowModeAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", pathToString( trackPath_ ) );
    elem.setAttribute( "mode", STrack::feelFlowModeToString( mode_ ) );
}

bool SSetFeelFlowModeAction::readXml( const QDomElement &elem, int /*version*/ )
{
    trackPath_ = stringToPath( elem.attribute( "trackPath", "0" ) );
    mode_ = STrack::feelFlowModeFromString( elem.attribute( "mode", "adaptive" ) );
    return true;
}

static const bool s_reg_set_feel_flow_mode = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "set-feel-flow-mode" ),
        [] { return new SSetFeelFlowModeAction; } ), true );
