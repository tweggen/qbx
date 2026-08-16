#include "app/objects/track/ssettrackmuteaction.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/actions/sactionregistry.h"
#include "app/model/sappcontext.h"
#include "app/model/sautomationlane.h"
#include "app/objects/track/sautomationactions.h"
#include <QDebug>
#include <memory>
#include <QDomElement>

using namespace strackpath;

// Resolve the addressed lane: path form first (it reaches nested lanes), then
// the legacy top-level index.
static SObject *laneFor( SProject *project, const QList<int> &path, int index )
{
    SObject *mixer = splacements::rootContainer( project );
    if( !mixer ) return nullptr;
    if( !path.isEmpty() ) return splacements::laneAt( mixer, path );
    if( index < 0 || index >= mixer->childCount() ) return nullptr;
    return splacements::laneAt( mixer, QList<int>{ index } );
}

SApplyResult SSetTrackMuteAction::apply( SProject *project )
{
    SObject *lane = laneFor( project, trackPath_, trackIndex_ );
    if( !lane ) {
        qWarning() << "set-track-mute: no track at"
                   << ( trackPath_.isEmpty() ? QString::number( trackIndex_ )
                                             : pathToString( trackPath_ ) );
        return {false, nullptr};
    }
    // A STATIC EDIT ON A READ LANE BECOMES A POINT (design §3.4 / D5). Without
    // it the history and the lane disagree: the button would move, the render
    // would not, and undo would carry a step nobody can hear. Trim and Off are
    // deliberately NOT redirected — there the structural mute is still the thing
    // being edited (§11 decision 3).
    //
    // The lane addresses the track by PATH, so the legacy index form is
    // converted first; a track that has a lane is by definition one a script
    // reached through the modern spelling anyway.
    if( sautomation::readLaneFor( lane, QStringLiteral( "self:Muted" ) ) ) {
        const QList<int> path = trackPath_.isEmpty() ? QList<int>{ trackIndex_ }
                                                     : trackPath_;
        const offset_t at = SAppContext::get().getGlobalLocatorPos();
        std::unique_ptr<SAction> pt( sautomation::pointAtLocatorAction(
            path, QStringLiteral( "self:Muted" ), at, muted_ ? 1.0 : 0.0 ) );
        SApplyResult r = pt->apply( project );
        if( r.applied ) return r;
    }

    const bool old = lane->isMuted();
    // setMuted() emits mutedChanged(); the summing parent (root mixer, or the
    // folder track above a nested lane) enforces it — mute is a property of the
    // summing CHANNEL and is never baked into the lane's own output.
    lane->setMuted( muted_ );

    // The inverse addresses the SAME way it was addressed, so an index-form
    // element undoes through the index form.
    SSetTrackMuteAction *inverse =
        trackPath_.isEmpty() ? new SSetTrackMuteAction( trackIndex_, old )
                             : new SSetTrackMuteAction( trackPath_, old );
    return {true, inverse};
}

void SSetTrackMuteAction::writeXml( QDomElement &elem ) const
{
    if( !trackPath_.isEmpty() ) {
        elem.setAttribute( "trackPath", pathToString( trackPath_ ) );
    } else {
        elem.setAttribute( "trackIndex", QString::number( trackIndex_ ) );
    }
    elem.setAttribute( "muted", muted_ ? "1" : "0" );
}

bool SSetTrackMuteAction::readXml( const QDomElement &elem, int /*version*/ )
{
    trackIndex_ = elem.attribute( "trackIndex", "0" ).toInt();
    trackPath_ = stringToPath( elem.attribute( "trackPath" ) );
    const QString v = elem.attribute( "muted", "0" );
    muted_ = ( v == "1" || v == "true" );
    return true;
}

static const bool s_reg_settrackmute = (
    SActionRegistry::instance().registerType(
        QStringLiteral("set-track-mute"),
        []{ return new SSetTrackMuteAction; }
    ), true
);
