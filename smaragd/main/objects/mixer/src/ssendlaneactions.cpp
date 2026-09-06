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

SRemoveSendLaneAction::~SRemoveSendLaneAction()
{
    // Discarded while still holding the removed lane (i.e. in the "removed"
    // state): let it go, its refcount reaches zero and it is torn down with
    // its plugin chain.
    dropStalePin();
}

void SRemoveSendLaneAction::dropStalePin()
{
    if( holdsRef_ && heldLane_ ) heldLane_->removeRef();
    heldLane_ = nullptr;
    holdsRef_ = false;
}

void SRemoveSendLaneAction::releaseHeld()
{
    dropStalePin();
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

    // PIN IT BEFORE DETACHING, and the ORDER is the whole mechanism.
    // detachSendLane() deletes the mixer's own SLink reference, which drops a
    // reference; taking ours first means the net count never touches zero, so
    // the lane and its entire plugin chain survive the removal intact. Release
    // any pin left from a previous apply first (a redo whose lane is now
    // orphaned) -- the same dropStalePin() discipline SRemoveTrackAction uses,
    // and for the same reason.
    dropStalePin();
    lane->addRef();
    heldLane_ = lane;
    holdsRef_ = true;

    mixer->detachSendLane( lane );
    SAppContext::get().rewireSpeaker();
    mixer->notifyTreeChanged();

    // THE INVERSE RE-ADOPTS THIS OBJECT, not a fresh lane of the same name.
    // Returning add-send-lane here -- which is what the first version did --
    // makes a removed send come back EMPTY on Ctrl-Z: its inserts, their state
    // and its fader are all gone. "Nothing can hear a send's chain yet" is an
    // argument about the AUDIO and not about the user's work, so it does not
    // excuse losing it.
    return { true, new SRestoreSendLaneAction( this, sendName_,
                                               arrangement_ ) };
}

// --- restore-send-lane ------------------------------------------------------

SRestoreSendLaneAction::SRestoreSendLaneAction( SRemoveSendLaneAction *owner,
                                                const QString &name,
                                                const QString &arrangement )
    : owner_( owner ), sendName_( name ), arrangement_( arrangement )
{
}

SApplyResult SRestoreSendLaneAction::apply( SProject *project )
{
    if( !owner_ ) return { false, nullptr };
    SStdMixer *mixer = mixerFor( project, arrangement_ );
    if( !mixer ) {
        qWarning() << "restore-send-lane: no mixer root for arrangement"
                   << arrangement_;
        return { false, nullptr };
    }
    STrack *lane = owner_->heldLane();
    if( !lane ) {
        qWarning() << "restore-send-lane: nothing pinned to restore for"
                   << sendName_;
        return { false, nullptr };
    }

    // Re-adopt, which mints a fresh SLink reference; only THEN release the
    // pin, so the count never touches zero between the two.
    mixer->adoptSendLane( lane );
    owner_->releaseHeld();

    SAppContext::get().rewireSpeaker();
    mixer->notifyTreeChanged();

    return { true, new SRemoveSendLaneAction( sendName_, arrangement_ ) };
}

void SRestoreSendLaneAction::writeXml( QDomElement &elem ) const
{
    // Never serialized standalone (created live as a removal's inverse); the
    // address is recorded for completeness.
    elem.setAttribute( "sendName", sendName_ );
    if( !arrangement_.isEmpty() ) elem.setAttribute( "arrangement", arrangement_ );
}

bool SRestoreSendLaneAction::readXml( const QDomElement &, int )
{
    // Not reconstructible from XML: there is no pinned lane in a file. Same
    // answer SRestoreTrackAction gives, for the same reason.
    return false;
}

// DELIBERATELY NOT REGISTERED, exactly as SRestoreTrackAction is not. It is
// created live as a removal's inverse and holds a POINTER to the action that
// pins the lane; there is no pinned object in a file, so readXml() refuses and
// a registry entry would only offer callers an action that can never be built.
// action_roundtrip_test caught the first draft's registration immediately --
// "Failed to deserialize action from XML" -- which is the audit working.

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
