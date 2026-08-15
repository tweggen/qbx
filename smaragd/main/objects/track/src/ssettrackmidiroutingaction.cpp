#include "app/objects/track/ssettrackmidiroutingaction.h"

#include <QDebug>
#include <QDomElement>

#include "app/actions/sactionregistry.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/objects/track/strack.h"

using namespace strackpath;

SSetTrackMidiRoutingAction::SSetTrackMidiRoutingAction(
    const QList<int> &trackPath, const QString &routing )
    : trackPath_( trackPath ), routing_( routing )
{
}

SApplyResult SSetTrackMidiRoutingAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };
    SObject *mixer = splacements::rootContainer( project );
    STrack *track = dynamic_cast<STrack *>(
        splacements::laneAt( mixer, trackPath_ ) );
    if( !track ) {
        qWarning() << "set-track-midi-routing: no track at"
                   << pathToString( trackPath_ );
        return { false, nullptr };
    }
    bool ok = false;
    const STrack::MidiRouting want =
        STrack::midiRoutingFromString( routing_, &ok );
    if( !ok ) {
        qWarning() << "set-track-midi-routing: expected auto|parent|none, got"
                   << routing_;
        return { false, nullptr };
    }
    const QString before = STrack::midiRoutingToString( track->getMidiRouting() );
    track->setMidiRouting( want );
    return { true, new SSetTrackMidiRoutingAction( trackPath_, before ) };
}

void SSetTrackMidiRoutingAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", pathToString( trackPath_ ) );
    elem.setAttribute( "routing", routing_ );
}

bool SSetTrackMidiRoutingAction::readXml( const QDomElement &elem, int )
{
    trackPath_ = stringToPath( elem.attribute( "trackPath" ) );
    routing_ = elem.attribute( "routing", "auto" );
    return true;
}

static const bool s_reg_set_track_midi_routing = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "set-track-midi-routing" ),
        []{ return new SSetTrackMidiRoutingAction; } ), true );
