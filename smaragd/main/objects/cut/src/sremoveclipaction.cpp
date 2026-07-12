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
                                      offset_t startTime )
    : clipPath_( clipPath ), sourceClipPath_( sourceClipPath ),
      destTrackPath_( destTrackPath ), startTime_( startTime )
{
}

SApplyResult SRemoveClipAction::apply( SProject *project )
{
    if( !project || clipPath_.isEmpty() ) {
        return {false, nullptr};
    }
    SObject *mixer = splacements::rootContainer( project );
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
    if( !link || (link->getSObject().isPathContainer() ) ) {
        return {false, nullptr};
    }
    delete link;   // the SCut becomes unreferenced -> deleteLater

    // Inverse re-duplicates the original onto the same spot.
    SAction *inverse = new SDuplicateClipAction( sourceClipPath_, destTrackPath_, startTime_ );
    return {true, inverse};
}

void SRemoveClipAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", pathToString( clipPath_ ) );
    elem.setAttribute( "source", pathToString( sourceClipPath_ ) );
    elem.setAttribute( "destTrack", pathToString( destTrackPath_ ) );
    elem.setAttribute( "startTime", QString::fromStdString( Fraction(startTime_, 1).toString() ) );
}

bool SRemoveClipAction::readXml( const QDomElement & /*elem*/, int /*version*/ )
{
    return false;   // created live as a duplicate's inverse; not reconstructed here
}
