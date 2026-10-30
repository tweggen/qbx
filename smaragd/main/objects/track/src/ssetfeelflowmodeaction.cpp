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
    // The root arrives as a parameter, like the other ~60 verbs (QBX-245).
    SObject *root = splacements::rootNamed( project, pathRoot_ );
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
    elem.setAttribute( "trackPath", qualifiedToString( pathRoot_, trackPath_ ) );
    elem.setAttribute( "mode", STrack::feelFlowModeToString( mode_ ) );
}

bool SSetFeelFlowModeAction::readXml( const QDomElement &elem, int /*version*/ )
{
    // parseInto, NOT stringToPath (QBX-245): stringToPath splits on commas
    // and silently yields a MASTER-rooted path from a qualified one, and this
    // file was wrong at BOTH ends -- it parsed master-rooted and then resolved
    // against the master, so the two halves agreed and the defect was
    // invisible. Its check_pathroot exemption is deleted in the same change.
    trackPath_ = parseInto( pathRoot_, elem.attribute( "trackPath", "0" ) );
    mode_ = STrack::feelFlowModeFromString( elem.attribute( "mode", "adaptive" ) );
    return true;
}

static const bool s_reg_set_feel_flow_mode = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "set-feel-flow-mode" ),
        [] { return new SSetFeelFlowModeAction; } ), true );
