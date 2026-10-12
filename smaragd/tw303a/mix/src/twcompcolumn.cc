#include "tw/mix/twcompcolumn.h"
#include "tw/events/twfade.h"

#include "tw/core/twlog.h"
#include "tw/graph/twview.h"
#include "tw/graph/twlatch.h"
#include "tw/graph/tw303aenv.h"
#include "tw/pages/tw_output_page.h"
#include "tw/pages/io_vector.h"

#include <algorithm>

twCompColumn::twCompColumn( tw303aEnvironment &env, idx_t channels )
    : twComponent( env ), env_( env ), channels_( channels < 1 ? 1 : channels )
{
}

twCompColumn::~twCompColumn() = default;

void twCompColumn::setTakes( std::vector<ResolveFn> takes )
{
    std::lock_guard<std::mutex> lock( mutex() );
    takes_.clear();
    takes_.reserve( takes.size() );
    for( ResolveFn &fn : takes ) {
        TakeEntry e;
        // The view resolves the take's component AND its mapped position from
        // ONE snapshot (proposal 19 Inv-1); the getComponent callback is the
        // position-INDEPENDENT half, and returning null there is what keeps a
        // structural query from triggering a lazy reader build.
        ResolveFn resolve = fn;
        e.view = std::make_shared<twView>(
            env_,
            // The position-INDEPENDENT half (structure, teardown): the take's
            // root component as it stands. It must NOT trigger a lazy reader
            // build, which is exactly what `getRootComponent` promises.
            [resolve]() {
                twResolvedClip r = resolve( 0 );
                return r.component;
            },
            [resolve]( offset_t p ) { return resolve( p ); } );
        e.view->init();
        // UNBOUNDED by default, so a caller that sets no extent behaves
        // exactly as every caller did before plan 53 §4.1. The model sets
        // them right after this call (STakeStack::ensureCompColumn).
        e.exStart = 0;
        e.exLen   = -1;
        takes_.push_back( std::move( e ) );
    }
}

void twCompColumn::setCompMap( const twCompMap &map, int activeTake )
{
    std::lock_guard<std::mutex> lock( mutex() );
    map_ = map;
    activeTake_ = activeTake;
}

void twCompColumn::setInputExtent( int takeIndex, offset_t tStart,
                                   length_t len )
{
    std::lock_guard<std::mutex> lock( mutex() );
    if( takeIndex < 0 || (size_t) takeIndex >= takes_.size() ) return;
    TakeEntry &e = takes_[ (size_t) takeIndex ];
    e.exStart = tStart < 0 ? 0 : tStart;
    e.exLen   = len;
}

bool twCompColumn::takeLiveAt_nolock( int t, offset_t pos ) const
{
    if( t < 0 || (size_t) t >= takes_.size() ) return false;
    const TakeEntry &e = takes_[ (size_t) t ];
    if( e.exLen < 0 ) return true;                  // unbounded: as before
    return pos >= e.exStart && pos < e.exStart + (offset_t) e.exLen;
}

offset_t twCompColumn::nextTakeEdgeAfter_nolock( int t, offset_t pos ) const
{
    if( t < 0 || (size_t) t >= takes_.size() ) return -1;
    const TakeEntry &e = takes_[ (size_t) t ];
    if( e.exLen < 0 ) return -1;
    if( pos < e.exStart ) return e.exStart;
    const offset_t end = e.exStart + (offset_t) e.exLen;
    return ( pos < end ) ? end : -1;
}

int twCompColumn::takeAt_nolock( offset_t pos ) const
{
    const int t = map_.takeAt( (int64_t) pos );
    const int chosen = ( t >= 0 ) ? t : activeTake_;
    return ( chosen >= 0 && chosen < (int) takes_.size() ) ? chosen : -1;
}

int64_t twCompColumn::clampedXfade_nolock( size_t segIndex ) const
{
    const std::vector<twCompSegment> &segs = map_.segments();
    if( segIndex == 0 || segIndex >= segs.size() ) return 0;
    int64_t x = segs[ segIndex ].xfade;
    if( x <= 0 ) return 0;
    // Half the distance to each neighbouring boundary: two crossfades may
    // TOUCH and can never overlap, so at most two takes are ever live.
    const int64_t prevGap = segs[ segIndex ].at - segs[ segIndex - 1 ].at;
    if( x > prevGap ) x = prevGap;
    if( segIndex + 1 < segs.size() ) {
        const int64_t nextGap = segs[ segIndex + 1 ].at - segs[ segIndex ].at;
        if( x > nextGap ) x = nextGap;
    }
    // AND TO BOTH TAKES' EXTENTS (plan 53 §4.1). The fade is centred on `at`,
    // so it spans [at - x/2, at + x/2): the OUTGOING take must have material
    // up to the right end and the INCOMING one from the left end. Each bound
    // is therefore twice the material that take has on its own side.
    const offset_t at = (offset_t) segs[ segIndex ].at;
    const int takeOut = takeAt_nolock( at - 1 );
    const int takeIn  = segs[ segIndex ].take;
    if( takeOut >= 0 && (size_t) takeOut < takes_.size() ) {
        const TakeEntry &e = takes_[ (size_t) takeOut ];
        if( e.exLen >= 0 ) {
            const int64_t room = 2 * (int64_t) ( e.exStart
                                                 + (offset_t) e.exLen - at );
            if( x > room ) x = room;
        }
    }
    if( takeIn >= 0 && (size_t) takeIn < takes_.size() ) {
        const TakeEntry &e = takes_[ (size_t) takeIn ];
        if( e.exLen >= 0 ) {
            const int64_t room = 2 * (int64_t) ( at - e.exStart );
            if( x > room ) x = room;
        }
    }
    return x < 0 ? 0 : x;
}

twCompColumn::CompRun twCompColumn::runAt_nolock( offset_t pos,
                                                  length_t limit ) const
{
    CompRun r;
    r.len = limit;
    r.takeA = takeAt_nolock( pos );

    const std::vector<twCompSegment> &segs = map_.segments();
    // Every position at which the contributing SET changes: a crossfade's two
    // ends, and a boundary with no crossfade.
    for( size_t i = 0; i < segs.size(); ++i ) {
        const int64_t x = clampedXfade_nolock( i );
        const offset_t at = (offset_t) segs[ i ].at;
        const offset_t xs = (offset_t) ( at - x / 2 );
        const offset_t xe = (offset_t) ( xs + x );

        if( x > 0 && pos >= xs && pos < xe ) {
            // INSIDE this crossfade: both takes are live for the rest of it.
            r.takeA  = takeAt_nolock( (offset_t) ( segs[ i ].at - 1 ) );
            r.takeB  = segs[ i ].take;
            r.xStart = xs;
            r.xLen   = x;
            r.len    = std::min( limit, (length_t) ( xe - pos ) );
            return r;
        }
        // Otherwise the next event after `pos` ends this run.
        const offset_t evt = ( x > 0 ) ? xs : at;
        if( evt > pos ) {
            r.len = std::min( r.len, (length_t) ( evt - pos ) );
            break;      // segments are sorted; no later one can be nearer
        }
    }
    // AN EXTENT EDGE ENDS A RUN TOO (plan 53 §4.1). Without this a run could
    // straddle the end of its own take's material: half of it would be read
    // from the take and half would be silence, while the page plan declared
    // one dependency for the whole span. Clipping here keeps every run wholly
    // inside or wholly outside each contributing take, which is what "a span
    // over which the set of contributing takes does not change" has to mean
    // once takes have extents — and it keeps planPage's walk and freezePage's
    // identical, which is proposal 19 Inv-1 extended to the plan.
    for( int t : { r.takeA, r.takeB } ) {
        const offset_t edge = nextTakeEdgeAfter_nolock( t, pos );
        if( edge > pos ) r.len = std::min( r.len, (length_t) ( edge - pos ) );
    }
    if( r.len < 0 ) r.len = 0;
    return r;
}

twPagePlan twCompColumn::planPage( offset_t pageStart )
{
    twPagePlan plan;
    std::lock_guard<std::mutex> lock( mutex() );
    // The scheduler ASSERTS a stamped epoch: an unstamped plan cannot be
    // compared against the pages it produces.
    plan.component = shared_from_this();
    plan.pageStart = pageStart;
    plan.epoch     = contentEpochNow();
    const length_t pageLen = (length_t) twOutputPage::FRAME_CAPACITY;

    // DECLARE EVERY TAKE THIS PAGE WILL ACTUALLY READ, and no others. The
    // scheduler binds exactly what a plan asks for, so a take left out is a
    // page the render will not have (a MISS, then a re-freeze), and a take
    // declared and not read is a page nothing consumes. The walk is the SAME
    // one freezePage makes, region by region, which is proposal 19 Inv-1
    // extended to the plan: plan and render cannot disagree because they ask
    // the same question in the same order.
    offset_t pos = pageStart;
    const offset_t end = pageStart + (offset_t) pageLen;
    while( pos < end ) {
        const CompRun run = runAt_nolock( pos, (length_t) ( end - pos ) );
        // BOTH takes across a crossfade, or the scheduler will not have the
        // page the fade's other half reads (proposal 43 §6).
        for( int t : { run.takeA, run.takeB } ) {
            if( t < 0 || (size_t) t >= takes_.size() ) continue;
            if( !takes_[ (size_t) t ].view ) continue;
            // NO DEPENDENCY OUTSIDE THE EXTENT (plan 53 §4.1). A take with no
            // material here contributes silence, and declaring it would have
            // the scheduler build a lazy reader for a window the take does not
            // have. The run never straddles an edge (runAt_nolock clips at
            // one), so one test at `pos` answers for the whole run.
            if( !takeLiveAt_nolock( t, pos ) ) continue;
            // ...and the take is addressed in ITS OWN domain, which starts at
            // its tStart in the column's. Zero for a full-extent take, so this
            // is the old `resolve( pos )` wherever extents are not in use.
            const offset_t inTake = pos - takes_[ (size_t) t ].exStart;
            twResolvedClip r = takes_[ (size_t) t ].view->resolve( inTake );
            if( r.component )
                plan.deps.push_back( twPageDep{ r.component, r.mappedPos } );
        }
        if( run.len <= 0 ) break;
        pos += (offset_t) run.len;
    }
    return plan;
}

std::shared_ptr<twOutputPage> twCompColumn::freezePage(
    offset_t startPos, const sample_t * /*inputData*/, uint64_t /*inputOffset*/,
    length_t /*inputLength*/, int sampleRate,
    std::shared_ptr<twOutputPage> /*previousPage*/ )
{
    std::lock_guard<std::mutex> lock( mutex() );

    // WIDTH, by hand (proposal 36 §7 trap 19): this override allocates its own
    // page and so bypasses twComponent::freezePage, the one place a page
    // normally learns its width.
    auto page = std::make_shared<twOutputPage>( (std::uint16_t) channels_ );
    page->startPosition = startPos;
    page->contentEpoch = contentEpochNow();

    const length_t pageLen = (length_t) page->channelFrames();
    offset_t pos = startPos;
    const offset_t end = startPos + (offset_t) pageLen;

    while( pos < end ) {
        const CompRun run = runAt_nolock( pos, (length_t) ( end - pos ) );
        if( run.len <= 0 ) break;

        // Freeze each contributing take AT ITS OWN POSITION: a take is a
        // window addressed in its own domain, which begins at its tStart in
        // the column's (plan 53 §4.1). tStart is 0 for a full-extent take, so
        // this is the offset-free freeze it has always been wherever extents
        // are not in use.
        //
        // A TAKE WITH NO MATERIAL HERE FREEZES NOTHING and the span fills with
        // silence below — §3.1 step 5: there is no fall-back to another take.
        auto freezeTake = [&]( int t ) -> std::shared_ptr<twOutputPage> {
            if( t < 0 || (size_t) t >= takes_.size() ) return nullptr;
            TakeEntry &e = takes_[ (size_t) t ];
            if( !e.view ) return nullptr;
            if( !takeLiveAt_nolock( t, pos ) ) return nullptr;
            auto sp = e.view->freezePage( pos - e.exStart, nullptr, 0, run.len,
                                          sampleRate, e.previousPage );
            // The per-take DSP state chain. A take renders only where its
            // regions are, so its chain has GAPS -- inherent to cutting
            // between sources, and the same situation twTrackMix's
            // non-contiguous clips are in.
            if( sp && sp->validFrames > 0 ) e.previousPage = sp;
            return sp;
        };
        auto pa = freezeTake( run.takeA );
        auto pb = ( run.takeB >= 0 ) ? freezeTake( run.takeB ) : nullptr;

        const offset_t dst = pos - startPos;
        for( idx_t c = 0; c < (idx_t) page->channels(); ++c ) {
            sample_t *dp = page->channelPtr( c ) + dst;
            // The source channel is CLAMPED, never assumed: a take's page
            // carries the width of ITS source, and a mono take must play on
            // every channel (proposal 36 4.4).
            const sample_t *sa = ( pa && pa->validFrames > 0 )
                ? pa->channelPtr( twPageClampChannel( *pa, c ) ) : nullptr;
            const sample_t *sb = ( pb && pb->validFrames > 0 )
                ? pb->channelPtr( twPageClampChannel( *pb, c ) ) : nullptr;
            const length_t na = sa ? std::min<length_t>(
                                        (length_t) pa->validFrames, run.len ) : 0;
            const length_t nb = sb ? std::min<length_t>(
                                        (length_t) pb->validFrames, run.len ) : 0;

            if( run.takeB < 0 || run.xLen <= 0 ) {
                for( length_t i = 0; i < run.len; ++i )
                    dp[ i ] = ( i < na ) ? sa[ i ] : (sample_t) 0;
                continue;
            }
            // THE CROSSFADE. EQUAL POWER, always: two of these sum to constant
            // power across the join, which is what a comp boundary wants, and
            // it is the SAME curve family a clip fade uses (twFadeShape) --
            // a crossfade is two fades that meet, and two curve families that
            // could disagree about the same join is one more than there
            // should be.
            for( length_t i = 0; i < run.len; ++i ) {
                const double t =
                    (double) ( pos + (offset_t) i - run.xStart ) / (double) run.xLen;
                const double gIn  = twClipFade::shaped( t, twFadeShape::EqualPower );
                const double gOut = twClipFade::shaped( 1.0 - t, twFadeShape::EqualPower );
                const double a = ( i < na ) ? (double) sa[ i ] : 0.0;
                const double b = ( i < nb ) ? (double) sb[ i ] : 0.0;
                dp[ i ] = (sample_t) ( a * gOut + b * gIn );
            }
        }
        pos += (offset_t) run.len;
    }

    page->validFrames = (uint32_t) pageLen;
    page->validAspects = twAspectPlayback;
    return page;
}

void twCompColumn::invalidatePagesInRange( offset_t start, offset_t end )
{
    twComponent::invalidatePagesInRange( start, end );
    std::lock_guard<std::mutex> lock( mutex() );
    // Our takes' own caches, and the state predecessors we hand them: an edit
    // inside the range must not resume from a page rendered before it. Same
    // duty twTrackMix has for its clip entries.
    //
    // IN EACH TAKE'S OWN DOMAIN (plan 53 §4.1): the range arrives in the
    // COLUMN's frames, and a take's cache is keyed in its own, which begins at
    // its tStart in ours. A range entirely outside a take's extent reaches
    // nothing of that take and must not drop its state chain either — that
    // would re-render material the edit cannot have touched. Both reduce to
    // the old unconditional call for a full-extent take, where tStart is 0 and
    // every range intersects.
    for( TakeEntry &e : takes_ ) {
        offset_t a = start, b = end;
        if( e.exLen >= 0 ) {
            const offset_t s = e.exStart;
            const offset_t t = e.exStart + (offset_t) e.exLen;
            if( a < s ) a = s;
            if( b > t ) b = t;
            if( b <= a ) continue;              // windowed away: nothing to do
        }
        if( e.view ) e.view->invalidatePagesInRange( a - e.exStart,
                                                    b - e.exStart );
        e.previousPage = nullptr;
    }
}

int twCompColumn::seekTo( offset_t off )
{
    std::lock_guard<std::mutex> lock( mutex() );
    // Each take is seeked in ITS OWN domain (see invalidatePagesInRange).
    for( TakeEntry &e : takes_ )
        if( e.view ) e.view->seekTo( off - e.exStart );
    return 0;
}

length_t twCompColumn::calcOutputTo( IOVector &dest, idx_t outChannel )
{
    // The LEGACY PULL path. A comp column is a freeze-path object: it is built
    // only when a map exists, and everything that reaches it (the mix, a
    // capture, a preview) goes through freezePage. Answering silence here is
    // honest and loud rather than half-rendering one take.
    (void) outChannel;
    TW_LOGW( "mix", "twCompColumn::calcOutputTo: the legacy pull path does not "
                    "render a comp map; use freezePage" );
    return dest.fillSilence( 0, dest.length() );
}

void twCompColumn::createOutputLatches()
{
    pOutputLatches_[0] =
        std::make_shared<twStreamingLatch>( shared_from_this(), 0, 0 );
}

void twCompColumn::reset()
{
    // The per-take state chains are what this component carries across pages;
    // dropping them is what a reset MEANS here. The takes' own components
    // reset themselves through their own views.
    for( TakeEntry &e : takes_ ) e.previousPage = nullptr;
}

void twCompColumn::teardown()
{
    std::lock_guard<std::mutex> lock( mutex() );
    for( TakeEntry &e : takes_ ) {
        if( e.view ) e.view->teardown();
        e.previousPage = nullptr;
    }
    takes_.clear();
}
