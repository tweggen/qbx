#include "app/objects/cut/srestoretakecolumnaction.h"

#include "app/actions/sactionregistry.h"
#include "app/model/slink.h"
#include "app/model/sobject.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/objects/cut/sremovesampleaction.h"
#include "tw/core/twlog.h"

#include <QDomElement>

SRestoreTakeColumnAction::SRestoreTakeColumnAction(
        SRemoveSampleAction *owner, const QList<int> &lanePath,
        offset_t timePos, int restoreAtIndex )
    : owner_( owner ), lanePath_( lanePath ), timePos_( timePos ),
      restoreAtIndex_( restoreAtIndex )
{
}

SApplyResult SRestoreTakeColumnAction::apply( SProject *project )
{
    if( !project || !owner_ ) {
        return {false, nullptr};
    }

    SObject *held = owner_->heldObject();
    if( !held ) {
        // Nothing pinned. Either the forward never pinned (a shape it could
        // rebuild instead) or the pin was already released by an earlier
        // apply of this inverse. Refusing is right: re-linking nothing would
        // report success for a clip that is not back.
        TW_LOGW( "cut", "restore-take-column: nothing pinned to restore" );
        return {false, nullptr};
    }

    SObject *root = splacements::rootNamed( project, pathRoot_ );
    SObject *lane = root ? splacements::placementLaneAt( root, lanePath_,
                                                         "restore-take-column" )
                         : nullptr;
    if( !lane ) {
        // The lane is genuinely gone -- the one refusal that is correct here.
        // KEEP THE PIN: the content is not lost, and the action's destructor
        // releases it. Note QUndoCommand::undo() returns void, so the stack
        // index moves regardless of this result; refusing for any other reason
        // would leave the next redo deleting whatever now sits at the stored
        // index, which is the QBX-149 failure class.
        TW_LOGW( "cut", "restore-take-column: no lane at the stored path" );
        return {false, nullptr};
    }

    // SLink ctor rule (slink.h): build with NO parent, then setParent, so
    // childEvent fires on a fully-constructed link.
    SLink *link = new SLink( *held, nullptr );
    link->setStartTime( timePos_ );
    link->setParent( lane );

    const int landed = lane->indexOfChild( link );
    if( landed < 0 ) {
        delete link;
        TW_LOGW( "cut", "restore-take-column: the re-linked clip did not land "
                        "in the lane" );
        return {false, nullptr};
    }

    // An inverse owes CHILD ORDER back, not merely the clip (QBX-149).
    // moveChildToIndex is a plain move of the explicit childOrder_ list and it
    // clamps, so a stale index lands at the nearest end rather than anywhere
    // illegal.
    if( restoreAtIndex_ >= 0 ) {
        lane->moveChildToIndex( landed, restoreAtIndex_ );
    }

    // The clip is attached, so the pin has done its job and the lane's link is
    // now the reference that keeps the content alive. Released AFTER the
    // re-link, never before: in between, nothing else holds it.
    owner_->releaseHeld();

    project->notifyArrangementChanged();

    // The forward re-pins on its next apply (it calls dropStalePin() first),
    // which is what makes undo -> redo -> undo work: SActionUndoCommand reuses
    // this same inverse object every time rather than building a fresh one.
    return {true, nullptr};
}

void SRestoreTakeColumnAction::writeXml( QDomElement & /*elem*/ ) const
{
    // Live-only action -- never serialized. It carries pointers to a live
    // action and a live SObject. Intentionally empty.
}

bool SRestoreTakeColumnAction::readXml( const QDomElement & /*elem*/,
                                        int /*version*/ )
{
    // Live-only action -- never deserialized.
    return false;
}
