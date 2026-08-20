#include "app/objects/track/ssettrackmidioutputaction.h"

#include <QDebug>
#include <QDomElement>

#include "app/actions/sactionregistry.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/objects/track/strack.h"

using namespace strackpath;

SSetTrackMidiOutputAction::SSetTrackMidiOutputAction(
    const QList<int> &trackPath, const QString &port, int channel, int offsetMs )
    : trackPath_( trackPath ), port_( port ), channel_( channel ),
      offsetMs_( offsetMs )
{
}

SApplyResult SSetTrackMidiOutputAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };
    SObject *mixer = splacements::rootNamed( project, pathRoot_ );
    STrack *track = dynamic_cast<STrack *>(
        splacements::laneAt( mixer, trackPath_ ) );
    if( !track ) {
        qWarning() << "set-track-midi-output: no track at"
                   << qualifiedToString( pathRoot_, trackPath_ );
        return { false, nullptr };
    }
    if( channel_ < -1 || channel_ > 15 ) {
        qWarning() << "set-track-midi-output: channel must be 0..15 (0-based)"
                   << "or -1 for 'as authored'; got" << channel_;
        return { false, nullptr };
    }
    if( offsetMs_ < -STrack::MIDI_OUT_MAX_OFFSET_MS
        || offsetMs_ > STrack::MIDI_OUT_MAX_OFFSET_MS ) {
        qWarning() << "set-track-midi-output: offsetMs must be within +-"
                   << STrack::MIDI_OUT_MAX_OFFSET_MS << "; got" << offsetMs_;
        return { false, nullptr };
    }

    // The inverse carries the whole previous state, because the verb is
    // absolute: a "restore the port only" inverse would silently keep the new
    // channel after an undo.
    SAction *inverse = new SSetTrackMidiOutputAction(
        trackPath_, track->getMidiOutPort(), track->getMidiOutChannel(),
        track->getMidiOutOffsetMs() );
    track->setMidiOutput( port_, channel_, offsetMs_ );
    return { true, inverse };
}

void SSetTrackMidiOutputAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", qualifiedToString( pathRoot_, trackPath_ ) );
    elem.setAttribute( "port", port_ );
    elem.setAttribute( "channel", QString::number( channel_ ) );
    elem.setAttribute( "offsetMs", QString::number( offsetMs_ ) );
}

bool SSetTrackMidiOutputAction::readXml( const QDomElement &elem, int )
{
    trackPath_ = parseInto( pathRoot_, elem.attribute( "trackPath" ) );
    port_      = elem.attribute( "port", "" );
    channel_   = elem.attribute( "channel", "-1" ).toInt();
    offsetMs_  = elem.attribute( "offsetMs", "0" ).toInt();
    return true;
}

static const bool s_reg_set_track_midi_output = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "set-track-midi-output" ),
        []{ return new SSetTrackMidiOutputAction; } ), true );
