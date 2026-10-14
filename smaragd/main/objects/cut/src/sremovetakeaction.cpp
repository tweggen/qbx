#include "app/objects/cut/sremovetakeaction.h"
#include "app/objects/cut/saddtakeaction.h"
#include "app/objects/cut/srestoretakeaction.h"
#include "app/objects/cut/stakestack.h"
#include "app/objects/cut/stakehelpers.h"
#include "app/model/sclipwindow.h"
#include "app/objects/cut/scut.h"        // pitch only (audio-specific)
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/model/slink.h"
#include "app/model/sexternfile.h"
#include "app/actions/sactionregistry.h"
#include <QDomElement>

using namespace strackpath;

SRemoveTakeAction::SRemoveTakeAction( const QList<int> &clipPath,
                                      int takeIndex, int thenActivate )
    : clipPath_( clipPath ), takeIndex_( takeIndex ),
      thenActivate_( thenActivate )
{
}

SApplyResult SRemoveTakeAction::apply( SProject *project )
{
    if( !project || clipPath_.isEmpty() ) {
        return {false, nullptr};
    }
    SObject *mixer = splacements::rootNamed( project, pathRoot_ );
    if( !mixer ) {
        return {false, nullptr};
    }
    QList<int> lanePath = clipPath_;
    int idx = lanePath.takeLast();
    SObject *lane = splacements::laneAt( mixer, lanePath );
    SLink *link = lane ? lane->childAt( idx ) : nullptr;
    if( !link ) {
        return {false, nullptr};
    }
    // BOTH SHAPES (proposal 42 M3): DIRECT-only, this was simply REJECTED on
    // a wrapped column -- `remove-take` did nothing at all there.
    STakeStack *stack = stakes::columnOfLink( link );
    if( !stack || takeIndex_ < 0 || takeIndex_ >= stack->nTakes() ) {
        return {false, nullptr};
    }
    SClipWindow *take = stack->takeAt( takeIndex_ );
    if( !take ) {
        return {false, nullptr};
    }

    // Capture the take's window for the inverse. Only file-backed takes are
    // restorable; anything else yields a non-undoable (but valid) removal.
    QString filePath;
    if( SExternFile *xf =
            dynamic_cast<SExternFile *>( &take->windowContent() ) )
        filePath = xf->getFileName();
    const offset_t off = take->startOffset();
    const Fraction stretch = take->stretchOrRate();
    // Pitch is audio-only (cut/CONTRACT invariant 7), so the inverse carries
    // it only for an audio take; add-take is an audio verb anyway.
    SCut *audioCut = dynamic_cast<SCut *>( &take->asObject() );
    const double pitchCents = audioCut ? audioCut->getPitchCents() : 0.0;
    const bool wasActive = ( stack->activeTakeIndex() == takeIndex_ );
    // THE REMOVED TAKE'S EXTENT, for the inverse (plan 53 §4.1; review 1 3c).
    // The inverse is an `add-take`, whose extent defaults are "the whole
    // column" -- so without these two numbers undoing the removal of a PARTIAL
    // take gave back a FULL-extent one. Read BEFORE removeTake, which takes
    // the take's link (and with it its tStart) away.
    const offset_t takeAt  = stack->takeExtentStart( takeIndex_ );
    const length_t takeLen = stack->takeExtentLength( takeIndex_ );
    // The WHOLE map as it stands BEFORE the removal (plan 53 B10). removeTake
    // renumbers it and drops the removed take's segments, and neither step is
    // reversible from the result: the dropped segments' positions and xfades
    // are gone. So the inverse carries the original rather than trying to
    // re-derive it.
    const twCompMap mapBefore = stack->compMap();
    // THE TAKE'S NAME, for the inverse. It is the ORIGIN MARKER
    // (`stakenames::isRecorded`), so an inverse that dropped it turned a
    // restored recording into an "original clip": exempt from `cleanup-takes`
    // for good, and invisible to `select-take take="newest"`. Measured in
    // cleanup_takes.qxa, where a restored take read `(untitled)` and the next
    // sweep kept what the first had removed.
    const QString takeNameBefore = take->asObject().getSName();

    // PIN A TAKE THAT NO FILE COULD REBUILD (plan 53 P6, QBX-184). `removeTake`
    // does `delete lk`, whose refcount release would deleteLater the window; an
    // extra reference held HERE -- on the persistent forward action, the way
    // SRemoveTrackAction holds a track -- keeps it and its children alive and
    // detached, so SRestoreTakeAction can put that very object back. An event
    // take's notes are its own children and nothing could rebuild them from a
    // path, which is why the file-less case needs identity rather than a recipe.
    //
    // Taken only when there is no file to rebuild from: where an `add-take`
    // inverse works it is the better one, because it SERIALIZES.
    dropStalePin();
    heldActive_ = stack->activeTakeIndex();
    if( filePath.isEmpty() ) {
        take->asObject().addRef();
        heldTake_ = take;
        holdsRef_ = true;
    }

    stack->removeTake( takeIndex_ );
    if( thenActivate_ >= -1 ) {
        stack->setActiveTake( thenActivate_ );
    }
    // Hand the comp map back, when the action that built this one cleared it
    // (plan 53 B4). AFTER setActiveTake, because a non-empty map is what
    // decides what sounds and must not be overwritten by the activation's own
    // bookkeeping; and before the collapse below, where the stack may be about
    // to go away and a map on it stops meaning anything.
    if( hasRestoreMap_ ) {
        stack->setCompMap( restoreMap_ );
    }

    // Invariant 3: a single remaining take collapses to a plain cut.
    SLink *resultLink = link;
    if( stack->nTakes() == 1 ) {
        resultLink = stakes::collapseSingleTakeStack( lane, link );
        if( !resultLink ) resultLink = link;
    }

    QList<int> invPath = lanePath;
    invPath.append( lane->indexOfChild( resultLink ) );
    if( filePath.isEmpty() ) {
        // THE REAL INVERSE for a take with no file behind it. It reads the
        // pinned window off this action, so it carries no copy of anything and
        // cannot go stale.
        if( !mapBefore.empty() ) setRestoreCompMap( mapBefore );
        return {true, heldTake_ ? new SRestoreTakeAction( this, invPath,
                                                          takeIndex_, wasActive,
                                                          takeAt, takeLen )
                                : nullptr};
    }
    SAddTakeAction *inverse =
        new SAddTakeAction( invPath, filePath, off, takeIndex_,
                            wasActive, stretch, pitchCents,
                            takeAt, takeLen, takeNameBefore );
    if( !mapBefore.empty() ) {
        inverse->setRestoreCompMap( mapBefore );
    }
    return {true, inverse};
}

SRemoveTakeAction::~SRemoveTakeAction()
{
    dropStalePin();
}

void SRemoveTakeAction::releaseHeld()
{
    dropStalePin();
}

void SRemoveTakeAction::dropStalePin()
{
    if( holdsRef_ && heldTake_ ) {
        heldTake_->asObject().removeRef();
    }
    heldTake_ = nullptr;
    holdsRef_ = false;
}

void SRemoveTakeAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", qualifiedToString( pathRoot_, clipPath_ ) );
    elem.setAttribute( "take", takeIndex_ );
    elem.setAttribute( "thenActivate", thenActivate_ );
}

bool SRemoveTakeAction::readXml( const QDomElement &elem, int /*version*/ )
{
    clipPath_ = parseInto( pathRoot_, elem.attribute( "clip" ) );
    takeIndex_ = elem.attribute( "take", "0" ).toInt();
    thenActivate_ = elem.attribute( "thenActivate", "-2" ).toInt();
    return true;
}

static const bool s_reg_removetake = (
    SActionRegistry::instance().registerType(
        QStringLiteral("remove-take"),
        []{ return new SRemoveTakeAction; }
    ), true
);
