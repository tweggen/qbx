#include "app/objects/track/ssettrackrecordmodeaction.h"
#include "app/objects/track/strack.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/model/sprojectprops.h"
#include "app/actions/sactionregistry.h"
#include <QDebug>
#include <QDomElement>

using namespace strackpath;

SApplyResult SSetTrackRecordModeAction::apply( SProject *project )
{
    SObject *mixer = splacements::rootNamed( project, pathRoot_ );
    if( !mixer ) {
        qWarning() << "set-track-record-mode: no root named" << pathRoot_;
        return { false, nullptr };
    }
    SObject *lane = splacements::laneAt( mixer, trackPath_ );
    // REFUSED ON A SYSTEM LANE, with arm-track / set-monitor-mode /
    // set-track-input (proposal 45 AC5.2). A record mode says how a RECORDING
    // lands, and nothing records onto the master, a send or the conductor --
    // the lane cannot even be armed. Accepting it would put a value in the
    // model that no reader may ever act on, which is the SStdMixer::volume_
    // defect those three verbs were guarded against.
    //
    // action_roundtrip_test's AC5.6 audit is a BLACKLIST, so a lane-addressing
    // verb with no row in kLaneRows silently accepts a system lane. This verb
    // declares Refuse there and record_mode_master_refused.qxa gates it.
    if( splacements::refuseSystemLane( lane, "set-track-record-mode" ) ) {
        return { false, nullptr };
    }
    STrack *track = dynamic_cast<STrack *>( lane );
    if( !track ) {
        qWarning() << "set-track-record-mode: no track at"
                   << qualifiedToString( pathRoot_, trackPath_ );
        return { false, nullptr };
    }
    // REFUSED, not coerced. An unknown value that silently meant "takes" would
    // pin the lane to a mode nobody chose and report success.
    if( !mode_.isEmpty() && !SProjectProps::isRecordMode( mode_ ) ) {
        qWarning() << "set-track-record-mode: refusing unknown mode" << mode_
                   << "- expected takes | takes-split | overdub | replace, or "
                      "empty to inherit";
        return { false, nullptr };
    }

    const QString was = track->getRecordMode();
    track->setRecordMode( mode_ );

    SSetTrackRecordModeAction *inverse =
        new SSetTrackRecordModeAction( trackPath_, was );
    inverse->pathRoot_ = pathRoot_;
    return { true, inverse };
}

void SSetTrackRecordModeAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", qualifiedToString( pathRoot_, trackPath_ ) );
    elem.setAttribute( "mode", mode_ );
}

bool SSetTrackRecordModeAction::readXml( const QDomElement &elem, int /*version*/ )
{
    trackPath_ = parseInto( pathRoot_, elem.attribute( "trackPath" ) );
    mode_      = elem.attribute( "mode", "" );
    return true;
}

static const bool s_reg_settrackrecordmode = (
    SActionRegistry::instance().registerType(
        QStringLiteral("set-track-record-mode"),
        []{ return new SSetTrackRecordModeAction; }
    ), true
);
