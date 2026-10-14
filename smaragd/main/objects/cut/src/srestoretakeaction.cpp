#include "app/objects/cut/srestoretakeaction.h"

#include <QDebug>
#include <QDomElement>

#include "app/model/sclipwindow.h"
#include "app/model/slink.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/objects/cut/sremovetakeaction.h"
#include "app/objects/cut/stakehelpers.h"
#include "app/objects/cut/stakestack.h"

SRestoreTakeAction::SRestoreTakeAction( SRemoveTakeAction *owner,
                                        const QList<int> &clipPath,
                                        int takeIndex, bool wasActive,
                                        offset_t extentStart,
                                        length_t extentLen )
    : owner_( owner ), clipPath_( clipPath ), takeIndex_( takeIndex ),
      wasActive_( wasActive ), extentStart_( extentStart ),
      extentLen_( extentLen )
{
}

SApplyResult SRestoreTakeAction::apply( SProject *project )
{
    if( !project || !owner_ || clipPath_.isEmpty() ) return { false, nullptr };
    SClipWindow *held = owner_->heldTake();
    if( !held ) {
        // The pin is gone, which means a previous apply already consumed it or
        // the owner released it. Refusing is right: re-inserting nothing would
        // report success over a column that did not get its take back.
        qWarning() << "restore-take: nothing pinned for" << clipPath_;
        return { false, nullptr };
    }

    SObject *mixer = splacements::rootNamed( project, pathRoot_ );
    if( !mixer ) return { false, nullptr };
    QList<int> lanePath = clipPath_;
    const int idx = lanePath.takeLast();
    SObject *lane = splacements::laneAt( mixer, lanePath );
    SLink *link = lane ? lane->childAt( idx ) : nullptr;
    if( !link ) return { false, nullptr };

    // THE COLUMN MAY HAVE COLLAPSED under the removal (invariant 3′: a single
    // remaining full-extent take becomes a plain cut again), so the placement
    // this inverse addresses can be a plain clip rather than a stack. Wrapping
    // it back is `add-take`'s own first step, and for the same reason.
    STakeStack *stack = stakes::columnOfLink( link );
    if( !stack ) {
        link = stakes::wrapCutLinkIntoStack( project, lane, link );
        stack = stakes::columnOfLink( link );
        if( !stack ) {
            qWarning() << "restore-take: not a column and not wrappable at"
                       << clipPath_;
            return { false, nullptr };
        }
    }

    // THE SAME OBJECT, back at its index with its extent. insertTake creates a
    // new SLink over it, which takes the reference the pin was standing in for.
    SLink *newLink = stack->insertTake( *held, takeIndex_, extentStart_,
                                        extentLen_ );
    if( !newLink ) {
        qWarning() << "restore-take: the column refused the take at"
                   << clipPath_;
        return { false, nullptr };
    }
    // ORDER MATTERS, and it is the removal's order read backwards: the map
    // decides what SOUNDS, so it goes on after the activation, whose own
    // bookkeeping would otherwise overwrite it (SRemoveTakeAction's own note).
    if( wasActive_ || owner_->heldActiveTake() >= 0 )
        stack->setActiveTake( wasActive_ ? takeIndex_
                                         : owner_->heldActiveTake() );
    if( owner_->hasHeldCompMap() )
        stack->setCompMap( owner_->heldCompMap() );

    stakes::publishColumnChange( newLink, stack );

    // The pin has done its job: the new link owns a reference now, and holding
    // a second one would keep the window alive past a later real deletion.
    owner_->releaseHeld();

    // No inverse. A redo re-runs the FORWARD remove-take, which takes its own
    // pin again -- the SRemoveTrackAction / SRestoreTrackAction pair works the
    // same way, and it is why the pin lives on the persistent forward action
    // rather than here.
    return { true, nullptr };
}

void SRestoreTakeAction::writeXml( QDomElement & ) const
{
    // Live-only (see the header): manufactured as an inverse, never scripted,
    // and a pin could not be written down anyway.
}

bool SRestoreTakeAction::readXml( const QDomElement &, int )
{
    return false;
}
