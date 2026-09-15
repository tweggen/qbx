#include "app/objects/track/ssettrackpanaction.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/actions/sactionregistry.h"
#include "app/objects/track/strack.h"
#include "tw/core/twlog.h"
#include <QDomElement>

using namespace strackpath;

SSetTrackPanAction::SSetTrackPanAction( const QList<int> &trackPath, double newPan )
    : trackPath_( trackPath ), newPan_( newPan )
{
}

SApplyResult SSetTrackPanAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };
    SObject *mixer = splacements::rootNamed( project, pathRoot_ );
    SObject *lane = splacements::laneAt( mixer, trackPath_ );
    STrack *track = dynamic_cast<STrack *>( lane );
    if( !track ) return { false, nullptr };

    // THE CONDUCTOR LANE CARRIES NO AUDIO (proposal 45, proposal 49 D2). A pan
    // there would be stored, shown and never heard.
    if( track->systemRole() == SSystemRole::Conductor ) {
        TW_LOGW( "track", "set-track-pan: refusing the conductor lane; it "
                          "carries no audio" );
        return { false, nullptr };
    }

    // PAN IS DEFINED AT WIDTH 2 ONLY (D1). 0 is always accepted. The value is
    // spelled with QString::number, not %f: this box's LC_NUMERIC is de_DE.
    if( newPan_ != 0.0 && project->channels() != 2 ) {
        TW_LOGW( "track", "set-track-pan: refusing pan %s on a %d-channel "
                          "project; pan is defined for 2 channels only",
                 qPrintable( QString::number( newPan_ ) ), project->channels() );
        return { false, nullptr };
    }

    const double oldPan = track->getPan();
    track->setPan( newPan_ );   // clamps to [-1, 1] itself (SObject::setPan)

    return { true, new SSetTrackPanAction( trackPath_, oldPan ) };
}

QString SSetTrackPanAction::mergeKey() const
{
    return QStringLiteral( "set-track-pan:%1" )
               .arg( qualifiedToString( pathRoot_, trackPath_ ) );
}

bool SSetTrackPanAction::mergeWith( const SAction *later )
{
    const SSetTrackPanAction *o = dynamic_cast<const SSetTrackPanAction *>( later );
    if( !o || o->trackPath_ != trackPath_ || o->pathRoot_ != pathRoot_ ) return false;
    newPan_ = o->newPan_;
    return true;
}

void SSetTrackPanAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", qualifiedToString( pathRoot_, trackPath_ ) );
    elem.setAttribute( "pan", QString::number( newPan_ ) );
}

bool SSetTrackPanAction::readXml( const QDomElement &elem, int /*version*/ )
{
    trackPath_ = parseInto( pathRoot_, elem.attribute( "trackPath" ) );
    // QString::toDouble is C-locale (trap T9); never strtod/atof here.
    newPan_ = elem.attribute( "pan", "0" ).toDouble();
    return true;
}

QStringList SSetTrackPanAction::knownAttributes() const
{
    return { QStringLiteral( "trackPath" ), QStringLiteral( "pan" ) };
}

static const bool s_reg_settrackpan = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "set-track-pan" ),
        []{ return new SSetTrackPanAction; }
    ), true
);
