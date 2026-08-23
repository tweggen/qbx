#include "tw/mix/twcompcolumn.h"

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
        takes_.push_back( std::move( e ) );
    }
}

void twCompColumn::setCompMap( const twCompMap &map, int activeTake )
{
    std::lock_guard<std::mutex> lock( mutex() );
    map_ = map;
    activeTake_ = activeTake;
}

int twCompColumn::takeAt_nolock( offset_t pos ) const
{
    const int t = map_.takeAt( (int64_t) pos );
    const int chosen = ( t >= 0 ) ? t : activeTake_;
    return ( chosen >= 0 && chosen < (int) takes_.size() ) ? chosen : -1;
}

length_t twCompColumn::runLength_nolock( offset_t pos, length_t limit ) const
{
    // The next boundary strictly after `pos` ends this run; nothing after it
    // does, because the map is sorted.
    for( const twCompSegment &s : map_.segments() ) {
        if( (offset_t) s.at > pos ) {
            const length_t run = (length_t) ( (offset_t) s.at - pos );
            return std::min( run, limit );
        }
    }
    return limit;
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
        const length_t run = runLength_nolock( pos, (length_t) ( end - pos ) );
        const int t = takeAt_nolock( pos );
        if( t >= 0 && takes_[ (size_t) t ].view ) {
            twResolvedClip r = takes_[ (size_t) t ].view->resolve( pos );
            if( r.component )
                plan.deps.push_back( twPageDep{ r.component, r.mappedPos } );
        }
        if( run <= 0 ) break;
        pos += (offset_t) run;
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
        const length_t run = runLength_nolock( pos, (length_t) ( end - pos ) );
        if( run <= 0 ) break;
        const int t = takeAt_nolock( pos );
        if( t >= 0 && takes_[ (size_t) t ].view ) {
            TakeEntry &e = takes_[ (size_t) t ];
            // Freeze the take at the SAME position: a take is a window
            // addressed in the COLUMN's own domain (take links have
            // startTime 0, stakestack.h), so there is no offset to apply and
            // nothing here has to know what kind of window it is.
            auto src = e.view->freezePage( pos, nullptr, 0, run, sampleRate,
                                           e.previousPage );
            if( src && src->validFrames > 0 ) {
                // The per-take DSP state chain. A take renders only where its
                // regions are, so its chain has GAPS -- which is inherent to
                // cutting between sources and is the same situation
                // twTrackMix's non-contiguous clips are in.
                e.previousPage = src;
                const offset_t dst = pos - startPos;
                const length_t n =
                    std::min<length_t>( (length_t) src->validFrames, run );
                for( idx_t c = 0; c < (idx_t) page->channels(); ++c ) {
                    // The source channel is CLAMPED, never assumed: a take's
                    // page carries the width of ITS source, and a mono take
                    // must play on every channel (proposal 36 §4.4).
                    const idx_t sc = twPageClampChannel( *src, c );
                    const sample_t *sp = src->channelPtr( sc );
                    sample_t *dp = page->channelPtr( c ) + dst;
                    for( length_t i = 0; i < n; ++i ) dp[ i ] = sp[ i ];
                }
            }
        }
        pos += (offset_t) run;
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
    for( TakeEntry &e : takes_ ) {
        if( e.view ) e.view->invalidatePagesInRange( start, end );
        e.previousPage = nullptr;
    }
}

int twCompColumn::seekTo( offset_t off )
{
    std::lock_guard<std::mutex> lock( mutex() );
    for( TakeEntry &e : takes_ )
        if( e.view ) e.view->seekTo( off );
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
