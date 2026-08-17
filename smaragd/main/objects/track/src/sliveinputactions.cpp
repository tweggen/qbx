#include "app/objects/track/sliveinputactions.h"

#include <QDebug>
#include <QDomElement>

#include "app/actions/sactionregistry.h"
#include "app/model/sappcontext.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/objects/track/strack.h"

using namespace strackpath;

namespace {

STrack *resolveTrack( SProject *project, const QList<int> &path, const char *verb )
{
    if( !project ) return nullptr;
    SObject *mixer = splacements::rootContainer( project );
    STrack  *track = dynamic_cast<STrack *>( splacements::laneAt( mixer, path ) );
    if( !track )
        qWarning() << verb << ": no track at" << pathToString( path );
    return track;
}

}  // namespace

// --- arm-track --------------------------------------------------------------

SArmTrackAction::SArmTrackAction( const QList<int> &trackPath, bool armed )
    : trackPath_( trackPath ), armed_( armed )
{
}

SApplyResult SArmTrackAction::apply( SProject *project )
{
    STrack *track = resolveTrack( project, trackPath_, "arm-track" );
    if( !track ) return { false, nullptr };

    SAction *inverse = new SArmTrackAction( trackPath_, track->isArmedForRecording() );
    track->setArmedForRecording( armed_ );
    // The live set is {armed && monitorEffective} u {monitor == on} (design
    // D9), so arming is a plan-rebuild trigger even when nothing else moved.
    SAppContext::get().liveLanesChanged();
    return { true, inverse };
}

void SArmTrackAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", pathToString( trackPath_ ) );
    elem.setAttribute( "armed", armed_ ? "1" : "0" );
}

bool SArmTrackAction::readXml( const QDomElement &elem, int )
{
    trackPath_ = stringToPath( elem.attribute( "trackPath" ) );
    const QString a = elem.attribute( "armed", "0" );
    armed_ = ( a == "1" || a.compare( "true", Qt::CaseInsensitive ) == 0 );
    return true;
}

// --- set-track-input --------------------------------------------------------

SSetTrackInputAction::SSetTrackInputAction( const QList<int> &trackPath,
                                            const QString &input )
    : trackPath_( trackPath ), input_( input )
{
}

SApplyResult SSetTrackInputAction::apply( SProject *project )
{
    STrack *track = resolveTrack( project, trackPath_, "set-track-input" );
    if( !track ) return { false, nullptr };

    // The spelling is validated but never NORMALISED: an unknown scheme is
    // refused here rather than stored and silently ignored later, because a
    // typo in `trackInput` is otherwise indistinguishable from an input that
    // does not happen to be plugged in.
    const QString v = input_.trimmed();
    if( !( v.isEmpty() || v == QStringLiteral( "none" )
           || v == QStringLiteral( "keyboard" )
           || v.startsWith( QStringLiteral( "audio:" ) )
           || v.startsWith( QStringLiteral( "midi:" ) ) ) ) {
        qWarning() << "set-track-input: unknown input spec" << input_
                   << "- expected none | audio:<device>:<mask> |"
                   << "midi:<port>:<ch|any> | keyboard";
        return { false, nullptr };
    }

    SAction *inverse = new SSetTrackInputAction( trackPath_, track->getTrackInput() );
    track->setTrackInput( v );
    SAppContext::get().liveLanesChanged();
    return { true, inverse };
}

void SSetTrackInputAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", pathToString( trackPath_ ) );
    elem.setAttribute( "input", input_ );
}

bool SSetTrackInputAction::readXml( const QDomElement &elem, int )
{
    trackPath_ = stringToPath( elem.attribute( "trackPath" ) );
    input_     = elem.attribute( "input", "" );
    return true;
}

// --- set-monitor-mode -------------------------------------------------------

SSetMonitorModeAction::SSetMonitorModeAction( const QList<int> &trackPath,
                                              STrack::MonitorMode mode )
    : trackPath_( trackPath ), mode_( mode )
{
}

SApplyResult SSetMonitorModeAction::apply( SProject *project )
{
    STrack *track = resolveTrack( project, trackPath_, "set-monitor-mode" );
    if( !track ) return { false, nullptr };

    SAction *inverse = new SSetMonitorModeAction( trackPath_, track->getMonitorMode() );
    track->setMonitorMode( mode_ );
    SAppContext::get().liveLanesChanged();
    return { true, inverse };
}

void SSetMonitorModeAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", pathToString( trackPath_ ) );
    elem.setAttribute( "mode", STrack::monitorModeToString( mode_ ) );
}

bool SSetMonitorModeAction::readXml( const QDomElement &elem, int )
{
    trackPath_ = stringToPath( elem.attribute( "trackPath" ) );
    bool ok = false;
    const STrack::MonitorMode m =
        STrack::monitorModeFromString( elem.attribute( "mode", "auto" ), &ok );
    if( !ok ) {
        qWarning() << "set-monitor-mode: unknown mode"
                   << elem.attribute( "mode" ) << "- expected auto | on | off";
        return false;
    }
    mode_ = m;
    return true;
}

static const bool s_reg_live_input_actions = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "arm-track" ), []{ return new SArmTrackAction; } ),
    SActionRegistry::instance().registerType(
        QStringLiteral( "set-track-input" ), []{ return new SSetTrackInputAction; } ),
    SActionRegistry::instance().registerType(
        QStringLiteral( "set-monitor-mode" ), []{ return new SSetMonitorModeAction; } ),
    true );
