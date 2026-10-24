#include "app/objects/cut/sremoveclipaction.h"
#include "app/model/splacements.h"
#include "app/objects/cut/sduplicateclipaction.h"
#include "app/model/sobjectpath.h"
#include "app/model/sproject.h"
#include "app/model/slink.h"
#include "tw/core/twfraction.h"
#include <QDomElement>

using namespace strackpath;

SRemoveClipAction::SRemoveClipAction( const QList<int> &clipPath,
                                      const QList<int> &sourceClipPath,
                                      const QList<int> &destTrackPath,
                                      offset_t startTime,
                                      bool share )
    : clipPath_( clipPath ), sourceClipPath_( sourceClipPath ),
      destTrackPath_( destTrackPath ), startTime_( startTime ),
      share_( share )
{
}

SApplyResult SRemoveClipAction::apply( SProject *project )
{
    if( !project || clipPath_.isEmpty() ) {
        return {false, nullptr};
    }
    SObject *mixer = splacements::rootNamed( project, pathRoot_ );
    if( !mixer ) {
        return {false, nullptr};
    }
    QList<int> trackPath = clipPath_;
    int idx = trackPath.takeLast();
    SObject *track = splacements::laneAt( mixer, trackPath );
    if( !track ) {
        return {false, nullptr};
    }
    SLink *link = track->childAt( idx );
    if( !link || (link->getSObject().isLane() ) ) {
        return {false, nullptr};
    }
    delete link;   // the SCut becomes unreferenced -> deleteLater

    // Inverse re-creates the original onto the same spot, as the SAME GESTURE
    // it was (QBX-217): a shared link as a shared link, a duplicate as a
    // duplicate. The two are indistinguishable in the model once made, so the
    // bit has to be carried rather than inferred.
    //
    // NOTE WHAT THIS DOES *NOT* DO: it is not how redo works. Redo re-applies
    // the stored forward action, and this inverse is deleted by
    // `SActionHistory::submit( …, skipHistory=true )` right after it is
    // returned. See the header — it is correctness in depth for the day
    // `remove-clip` is submitted forward, and it is deliberately not gated.
    SAction *inverse = share_
        ? (SAction *) new SLinkClipAction( sourceClipPath_, destTrackPath_, startTime_ )
        : (SAction *) new SDuplicateClipAction( sourceClipPath_, destTrackPath_, startTime_ );
    return {true, inverse};
}

void SRemoveClipAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", qualifiedToString( pathRoot_, clipPath_ ) );
    elem.setAttribute( "source", qualifiedToString( pathRoot_, sourceClipPath_ ) );
    elem.setAttribute( "destTrack", qualifiedToString( pathRoot_, destTrackPath_ ) );
    elem.setAttribute( "startTime", QString::fromStdString( Fraction(startTime_, 1).toString() ) );
}

bool SRemoveClipAction::readXml( const QDomElement & /*elem*/, int /*version*/ )
{
    return false;   // created live as a duplicate's inverse; not reconstructed here
}
