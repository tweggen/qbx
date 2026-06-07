#include "actions/sremoveclipaction.h"
#include "actions/sduplicateclipaction.h"
#include "actions/strackpath.h"
#include "sproject.h"
#include "sstdmixer.h"
#include "strack.h"
#include "slink.h"
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
    SStdMixer *mixer = dynamic_cast<SStdMixer*>( project->getRootComponent() );
    if( !mixer ) {
        return {false, nullptr};
    }
    QList<int> trackPath = clipPath_;
    int idx = trackPath.takeLast();
    STrack *track = dynamic_cast<STrack*>( resolveByPath( mixer, trackPath ) );
    if( !track ) {
        return {false, nullptr};
    }
    SLink *link = track->childAt( idx );
    if( !link || dynamic_cast<STrack*>( &link->getSObject() ) ) {
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
    elem.setAttribute( "startTime", QString::number( (double) startTime_ ) );
}

bool SRemoveClipAction::readXml( const QDomElement & /*elem*/, int /*version*/ )
{
    return false;   // created live as a duplicate's inverse; not reconstructed here
}
