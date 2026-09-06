#include "app/objects/mixer/ssendlaneactions.h"

#include <QDebug>
#include <QDomElement>

#include "app/actions/sactionregistry.h"
#include "app/model/sappcontext.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/objects/track/strack.h"

namespace {

SStdMixer *mixerFor( SProject *project, const QString &arrangement )
{
    if( !project ) return nullptr;
    return dynamic_cast<SStdMixer *>(
        splacements::rootNamed( project, arrangement ) );
}

}  // namespace

// --- add-send-lane ----------------------------------------------------------

SAddSendLaneAction::SAddSendLaneAction( const QString &name,
                                        const QString &arrangement )
    : sendName_( name ), arrangement_( arrangement )
{
}

SApplyResult SAddSendLaneAction::apply( SProject *project )
{
    SStdMixer *mixer = mixerFor( project, arrangement_ );
    if( !mixer ) {
        qWarning() << "add-send-lane: no mixer root for arrangement"
                   << arrangement_;
        return { false, nullptr };
    }
    if( sendName_.trimmed().isEmpty() ) {
        qWarning() << "add-send-lane: refused, a send lane needs a name -- the"
                   << "name IS the address ($send:<name>)";
        return { false, nullptr };
    }
    // AC7.5: a collision is REFUSED, never uniquified. Silently renaming a
    // user's send to "Reverb 2" would leave every path they wrote addressing
    // the other lane.
    if( mixer->sendLaneNamed( sendName_ ) ) {
        qWarning() << "add-send-lane: refused, a send lane called"
                   << sendName_ << "already exists in this arrangement";
        return { false, nullptr };
    }

    STrack *lane = mixer->addSendLane( sendName_ );
    if( !lane ) return { false, nullptr };

    // The engine graph root gained an input; mirrors SAddTrackAction.
    SAppContext::get().rewireSpeaker();
    mixer->notifyTreeChanged();

    return { true, new SRemoveSendLaneAction( sendName_, arrangement_ ) };
}

void SAddSendLaneAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "sendName", sendName_ );
    if( !arrangement_.isEmpty() ) elem.setAttribute( "arrangement", arrangement_ );
}

bool SAddSendLaneAction::readXml( const QDomElement &elem, int )
{
    sendName_    = elem.attribute( "sendName" );
    arrangement_ = elem.attribute( "arrangement" );
    return true;
}

static const bool s_reg_add_send_lane = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "add-send-lane" ),
        []{ return new SAddSendLaneAction; } ), true );

// --- remove-send-lane -------------------------------------------------------

SRemoveSendLaneAction::SRemoveSendLaneAction( const QString &name,
                                              const QString &arrangement )
    : sendName_( name ), arrangement_( arrangement )
{
}

SApplyResult SRemoveSendLaneAction::apply( SProject *project )
{
    SStdMixer *mixer = mixerFor( project, arrangement_ );
    if( !mixer ) {
        qWarning() << "remove-send-lane: no mixer root for arrangement"
                   << arrangement_;
        return { false, nullptr };
    }
    STrack *lane = mixer->sendLaneNamed( sendName_ );
    if( !lane ) {
        qWarning() << "remove-send-lane: no send lane called" << sendName_;
        return { false, nullptr };
    }

    // ONLY THE LAST ONE, and the refusal is announced rather than being a
    // silent no-op. The sentinel `-2 - k` IS the address, so removing a lane
    // from the middle shifts every lane after it and silently re-points every
    // path that named one -- exactly the "resolved against the wrong object
    // SUCCEEDS" class the sentinel exists to close. A general removal belongs
    // with the routing milestone, where a send can have something pointing AT
    // it that would have to be re-pointed too.
    const QList<STrack *> sends = mixer->sendLanes();
    if( sends.isEmpty() || sends.last() != lane ) {
        qWarning() << "remove-send-lane: refused," << sendName_
                   << "is not the LAST send lane; removing it would shift the"
                   << "$sendN address of every lane after it";
        return { false, nullptr };
    }

    mixer->detachSendLane( lane );
    SAppContext::get().rewireSpeaker();
    mixer->notifyTreeChanged();

    // THE INVERSE RE-CREATES BY NAME, NOT BY POINTER, and that is a real
    // limitation rather than an oversight: a send lane removed and restored
    // comes back EMPTY, losing any inserts it carried. Pinning the object the
    // way SRemoveTrackAction does is the correct fix and is deliberately not
    // done here -- M7 builds the shape, nothing can feed a send, and a lane
    // whose only content is inserts nobody can hear yet is not worth a second
    // restore-action class. Named in the milestone's own "NOT gated" list so
    // it is not discovered later.
    return { true, new SAddSendLaneAction( sendName_, arrangement_ ) };
}

void SRemoveSendLaneAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "sendName", sendName_ );
    if( !arrangement_.isEmpty() ) elem.setAttribute( "arrangement", arrangement_ );
}

bool SRemoveSendLaneAction::readXml( const QDomElement &elem, int )
{
    sendName_    = elem.attribute( "sendName" );
    arrangement_ = elem.attribute( "arrangement" );
    return true;
}

static const bool s_reg_remove_send_lane = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "remove-send-lane" ),
        []{ return new SRemoveSendLaneAction; } ), true );
