#include "app/model/sautofade.h"

#include "app/model/sclipwindow.h"
#include "app/model/slink.h"
#include "app/model/sobject.h"
#include "app/model/sproject.h"
#include "app/model/sprojectprops.h"

#include <algorithm>
#include <cmath>

namespace sautofade {

double msOf( const SProject *project )
{
    if( !project ) return 0.0;
    bool ok = false;
    const double ms = project->prop( QString::fromLatin1(
                                         SProjectProps::AutoFadeMs ),
                                     SProjectProps::autoFadeMsDefault() )
                          .toDouble( &ok );
    // A key that is there but is not a number is as good as absent: the
    // alternative is a NaN travelling into a frame count, and every
    // comparison against a NaN is false (the qIsFinite rule this project
    // already holds for guards).
    if( !ok || !std::isfinite( ms ) || ms <= 0.0 )
        return 0.0;
    return ms;
}

length_t framesOf( const SProject *project )
{
    const double ms = msOf( project );
    if( ms <= 0.0 ) return 0;
    const int rate = project->getSRate();
    if( rate <= 0 ) return 0;
    const double frames = ms * (double) rate / 1000.0;
    // Round to the nearest frame and never down to nothing: a project that
    // says it wants an auto-fade gets at least one frame of one, so the
    // feature cannot be silently off at a low sample rate.
    length_t n = (length_t) std::llround( frames );
    if( n < 1 ) n = 1;
    return n;
}

void seedInto( SProject *project )
{
    if( !project ) return;
    project->setProp( QString::fromLatin1( SProjectProps::AutoFadeMs ),
                      SProjectProps::autoFadeMsSeed() );
}

bool isSeamless( SClipWindow &left, SClipWindow &right )
{
    // The same content OBJECT, not an equal path (see the header).
    if( &left.windowContent() != &right.windowContent() ) return false;
    // A loop seam is a discontinuity whatever abuts it, and
    // timelineToSourceExact maps within one repetition.
    if( left.isLooping() || right.isLooping() ) return false;
    // Exactly the same time scaling; a Fraction, compared by value.
    if( left.stretchOrRate() != right.stretchOrRate() ) return false;
    const length_t leftLen = left.duration();
    if( leftLen <= 0 ) return false;
    // ...and the source positions meet, exactly, in the content's own units.
    return left.timelineToSourceExact( Fraction( (int64_t) leftLen ) )
        == right.contentAnchorExact();
}

twClipFade effectiveFade( const twClipFade &stored, length_t clipLen,
                          length_t autoFrames, bool seamlessAtStart,
                          bool seamlessAtEnd )
{
    twClipFade eff = stored;
    if( autoFrames <= 0 || clipLen <= 0 ) return eff;
    // min( autoFade, clipLen / 2 ): a clip shorter than two auto-fades still
    // reaches unity in the middle rather than becoming a triangle.
    const int64_t len = std::min<int64_t>( (int64_t) autoFrames,
                                           (int64_t) clipLen / 2 );
    if( len <= 0 ) return eff;
    if( eff.inLen  <= 0 && eff.autoIn  && !seamlessAtStart ) eff.inLen  = len;
    if( eff.outLen <= 0 && eff.autoOut && !seamlessAtEnd   ) eff.outLen = len;
    return eff;
}

namespace {

/// One clip of a lane, resolved once: its placement span and the window whose
/// parameters that placement carries.
struct FadeClip {
    SLink       *link  = nullptr;
    offset_t     start = 0;
    length_t     len   = 0;
    SClipWindow *win   = nullptr;   // null when the placement carries none
};

/// The two placement-level preconditions from `laneSeams`'s documentation,
/// and then the window question.
bool seamlessJoin( const FadeClip &left, const FadeClip &right )
{
    if( !left.win || !right.win ) return false;
    if( left.len <= 0 || right.len <= 0 ) return false;
    if( left.start + (offset_t) left.len != right.start ) return false;
    if( (length_t) left.win->durationBlocking()  != left.len )  return false;
    if( (length_t) right.win->durationBlocking() != right.len ) return false;
    return isSeamless( *left.win, *right.win );
}

/// Every lane under `obj`, `obj` included. Depth-first over childLinks(),
/// which is where lanes live whether they are a mixer's tracks, a folder
/// track's children or a fragment's.
void collectLanes( SObject *obj, QList<SObject *> &out )
{
    if( !obj ) return;
    if( obj->isLane() && !out.contains( obj ) ) out.append( obj );
    for( SLink *lk : obj->childLinks() ) {
        if( !lk ) continue;
        collectLanes( &lk->getSObject(), out );
    }
}

} // namespace

QHash<SLink *, Seams> laneSeams( SObject &lane )
{
    QList<FadeClip> clips;
    for( SLink *lk : lane.childLinks() ) {
        if( !lk || !lk->hasStartTime() ) continue;
        SObject *o = &lk->getSObject();
        if( o->isLane() ) continue;            // a nested lane, not a clip
        FadeClip fc;
        fc.link  = lk;
        fc.start = lk->getStartTime();
        // THE BLOCKING DURATION, because this is an EDIT path. `getDuration()`
        // takes the try-lock snapshot and falls back to `lastGoodSnapshot_`,
        // which under background-worker mutex contention answers with a
        // PRE-EDIT duration (proposal 19 Phase 2b; cut/CONTRACT inv. 6's
        // neighbourhood). That is exactly how the `takes_group_broadcast`
        // flake happened: a stale duration propagated into
        // `twTrackMix::updateClip` and left clip windows wrong. Here it would
        // make the abutment test and the clamp answer about a length the clip
        // no longer has, so a fade would appear or vanish for one refresh --
        // the same flake shape, in the audio.
        //
        // Safe to block: this runs on the main thread by contract (the one
        // main-thread funnel a clip's gain travels), never on the RT thread.
        fc.len   = o->getDurationBlocking();
        fc.win   = SClipWindow::parametersOf( *o );
        clips.append( fc );
    }
    std::sort( clips.begin(), clips.end(),
               []( const FadeClip &a, const FadeClip &b ) {
                   return a.start < b.start;
               } );

    QHash<SLink *, Seams> out;
    out.reserve( clips.size() );
    for( int i = 0; i < clips.size(); ++i ) {
        Seams sm;
        if( i > 0 )                 sm.in  = seamlessJoin( clips[ i - 1 ],
                                                           clips[ i ] );
        if( i + 1 < clips.size() )  sm.out = seamlessJoin( clips[ i ],
                                                           clips[ i + 1 ] );
        out.insert( clips[ i ].link, sm );
    }
    return out;
}

void reapplyAll( SProject *project )
{
    if( !project ) return;

    QList<SObject *> roots;
    if( SObject *master = project->getRootComponent() ) roots.append( master );
    for( SObject *arr : project->arrangements() )
        if( arr && !roots.contains( arr ) ) roots.append( arr );

    QList<SObject *> lanes;
    for( SObject *root : roots ) collectLanes( root, lanes );

    for( SObject *lane : lanes ) {
        // ORDER MATTERS. The bump is what re-reads every clip's fade through
        // STrack::refreshClipGainCurves and pushes the new lengths into the
        // mix; the invalidation is what makes the pages holding the old ones
        // go stale. Invalidating first would stale pages and then re-freeze
        // them from the unchanged mix, and the change would be inaudible
        // until some other edit happened to bump the chain — the silent
        // failure shape proposal 45 D3a names (SStdMixer::bumpMasterChainEpoch).
        lane->bumpRenderChainEpoch();
        lane->invalidateRenderPath();
    }
}

} // namespace sautofade
