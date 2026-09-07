#include "app/objects/mixer/strackbroadcast.h"

#include <algorithm>

#include "app/objects/track/strackpath.h"
#include "app/objects/mixer/slaneorder.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/objects/track/strack.h"

namespace strackbroadcast {

QList<STrack *> orderByLane( SStdMixer *mixer, const QList<STrack *> &in )
{
    QList<STrack *> out = in;
    if( !mixer ) return out;

    // The ARRANGER's options, deliberately: this is the order the user SEES,
    // and the two consumers that depend on it (`newTrackReference_`, and
    // indent/outdent's directional loops) are the arranger's own.
    const QVector<slaneorder::Lane> lanes =
        slaneorder::flattenTrackLanes( mixer, slaneorder::Options() );

    std::stable_sort( out.begin(), out.end(),
                      [&lanes]( STrack *l, STrack *r ) {
                          const int li = slaneorder::indexOfTrack( lanes, l );
                          const int ri = slaneorder::indexOfTrack( lanes, r );
                          // Tracks with no visible lane sort last, stably.
                          if( li < 0 ) return false;
                          if( ri < 0 ) return true;
                          return li < ri;
                      } );
    return out;
}

QList<STrack *> pruneNestedTargets( const QList<STrack *> &in )
{
    QList<STrack *> out;
    for( STrack *t : in ) {
        bool covered = false;
        for( STrack *other : in ) {
            if( other == t ) continue;
            // isSelfOrDescendant( candidate, ancestor ): is t below other?
            if( strackpath::isSelfOrDescendant( t, other ) ) { covered = true; break; }
        }
        if( !covered ) out.append( t );
    }
    return out;
}

QList<STrack *> targetsFor( SStdMixer *mixer, STrack *clicked )
{
    QList<STrack *> out;
    if( !clicked ) return out;
    // THE rule: only a gesture aimed INTO the selection broadcasts. Aiming at a
    // lane outside it acts on that lane alone, so an operation can never reach
    // a track the user is not pointing at.
    if( mixer && mixer->isTrackSelected( clicked )
        && mixer->nSelectedTracks() > 1 ) {
        return orderByLane( mixer, mixer->getSelectedTracks() );
    }
    out.append( clicked );
    return out;
}

}  // namespace strackbroadcast
