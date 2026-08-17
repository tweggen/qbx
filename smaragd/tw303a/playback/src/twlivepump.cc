#include "tw/playback/twlivepump.h"

#include "tw/core/twlog.h"
#include "tw/graph/tw_freeze_context.h"
#include "tw/graph/twcomponent.h"
#include "tw/pages/tw_output_page.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>

#if defined( _WIN32 )
#  include <windows.h>
#  include <avrt.h>
#endif

namespace {
// The idle wait between "the ring is full" and the next attempt. The RT's DRAIN
// is the pacer — the pump renders whenever there is room and sleeps otherwise —
// so this is a granularity, not a schedule: a ring 4 blocks deep at 1024 frames
// is ~85 ms of slack at 48 kHz, and Windows' 15.6 ms scheduler tick fits inside
// it with room to spare. (Proposal 21 L0's PreciseWaiter is the sub-millisecond
// answer for a thread that must hit a DEADLINE; this one only has to not spin.)
constexpr int kIdleWaitMs = 2;
}  // namespace

LiveGraphPump::LiveGraphPump( twLiveMixRing &ring, const twEngineClock &clock )
    : ring_( ring ), clock_( clock )
{
}

LiveGraphPump::~LiveGraphPump()
{
    stop();
}

void LiveGraphPump::setPlan( std::shared_ptr<const twLivePlan> plan )
{
    {
        std::lock_guard<std::mutex> lock( planMutex_ );
        plan_ = std::move( plan );
    }
    planGen_.fetch_add( 1, std::memory_order_release );
}

std::shared_ptr<const twLivePlan> LiveGraphPump::plan() const
{
    std::lock_guard<std::mutex> lock( planMutex_ );
    return plan_;
}

void LiveGraphPump::resetStats()
{
    blocks_.store( 0, std::memory_order_relaxed );
    repositions_.store( 0, std::memory_order_relaxed );
    frozenMisses_.store( 0, std::memory_order_relaxed );
    shortfalls_.store( 0, std::memory_order_relaxed );
    ringFull_.store( 0, std::memory_order_relaxed );
}

// --------------------------------------------------------------- the thread

void LiveGraphPump::start()
{
    if( running_.load( std::memory_order_acquire ) ) return;
    stopFlag_.store( false, std::memory_order_relaxed );
    running_.store( true, std::memory_order_release );
    thread_ = std::thread( [this] { loop(); } );
}

void LiveGraphPump::stop()
{
    if( !running_.load( std::memory_order_acquire ) && !thread_.joinable() ) return;
    stopFlag_.store( true, std::memory_order_release );
    if( thread_.joinable() ) thread_.join();
    running_.store( false, std::memory_order_release );
}

void LiveGraphPump::loop()
{
    // The two markers, in this order and once: markLiveThread() sets the
    // per-thread render policy to Never (a freezePage from here is silence plus
    // a counter, never a block on a mutex and a disk read) AND marks the log
    // sink non-blocking, exactly as the RT callback does.
    twRtThreadGuard::markLiveThread();

#if defined( _WIN32 )
    // Best-effort, exactly like WASAPIBackend's render thread. A pump that gets
    // preempted by a browser tab produces the same audio, late; MMCSS is what
    // stops "late" from being the common case.
    DWORD  mmcssIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW( L"Pro Audio", &mmcssIndex );
#endif

    while( !stopFlag_.load( std::memory_order_acquire ) ) {
        // Fill the ring while there is room; the RT's drain paces us.
        bool produced = false;
        while( renderOneBlock() ) {
            produced = true;
            if( stopFlag_.load( std::memory_order_acquire ) ) break;
        }
        if( stopFlag_.load( std::memory_order_acquire ) ) break;
        (void)produced;
        std::this_thread::sleep_for( std::chrono::milliseconds( kIdleWaitMs ) );
    }

#if defined( _WIN32 )
    if( mmcss ) AvRevertMmThreadCharacteristics( mmcss );
#endif
}

// ---------------------------------------------------------------- one block

bool LiveGraphPump::renderOneBlock()
{
    // The harness calls this on ITS thread, so the marker belongs here and not
    // only in loop(): the ownership guard identifies "the pump" by exactly this
    // per-thread flag, and a synchronous harness must be the pump for the
    // duration of the call or every render would be refused.
    twRtThreadGuard::markLiveThread();

    // --- adopt a new plan --------------------------------------------------
    const std::uint64_t gen = planGen_.load( std::memory_order_acquire );
    if( gen != heldGen_ ) {
        std::shared_ptr<const twLivePlan> next;
        {
            std::lock_guard<std::mutex> lock( planMutex_ );
            next = plan_;
        }
        // The OLD plan dies HERE, at the top of a block, with nothing of it in
        // flight — which is what "released after the pump's next block" means.
        held_    = std::move( next );
        heldGen_ = gen;

        // Per-plan state. Allocated ON ADOPTION, never inside a block.
        heldPages_.clear();
        pageBase_.clear();
        resultBuf_.clear();
        std::size_t nPages = 0, widest = 0;
        if( held_ ) {
            pageBase_.reserve( held_->tracks.size() );
            resultBuf_.assign( held_->tracks.size(), 0 );
            for( const twLiveTrackPlan &t : held_->tracks ) {
                pageBase_.push_back( nPages );
                nPages += t.frozenInputs.size();
                widest = std::max( widest, (std::size_t)t.channels );
            }
        }
        heldPages_.assign( nPages, nullptr );
        inPtrs_.assign( widest, nullptr );
        outPtrs_.assign( widest, nullptr );

        // THE TRANSPORT IS APPLIED FROM THE PLAN, once per adoption. The plan
        // builder decides it (design D2); pushing it through here is what makes
        // it impossible for the plan and the processors to disagree about
        // whether the transport is running, which they otherwise could for one
        // block after every rebuild. One bounded lock per processor, on a plan
        // swap, never inside a block.
        if( held_ )
            for( const twLiveTrackPlan &t : held_->tracks )
                for( const std::shared_ptr<audio::twPluginSlotProcessor> &s : t.inserts )
                    if( s ) s->setLiveTransport( held_->transport );
        // POSITION STATE SURVIVES A SWAP. A plan is rebuilt for a fader move or
        // a device change as readily as for an arm, and forcing a reposition on
        // every rebuild would click for no reason. The FIRST adoption still
        // repositions, because havePos_ is false.
    }

    const twLivePlan *plan = held_.get();
    if( !plan || plan->empty() ) return false;

    const length_t block = plan->blockFrames;
    if( block <= 0 ) return false;

    float *dest = ring_.beginWrite();
    if( !dest ) {
        ringFull_.fetch_add( 1, std::memory_order_relaxed );
        return false;   // the RT is not draining: DROP, never block or grow
    }

    // --- the live clock (design D2) ----------------------------------------
    const bool playing = plan->transport.playing;
    bool reposition = !havePos_ || ( havePos_ && playing != wasPlaying_ );
    offset_t want = havePos_ ? nextPos_ : plan->stoppedAnchor;

    if( playing ) {
        const twEnginePosition p = clock_.read();
        if( p.valid() ) {
            want = (offset_t)p.deliveredFrame + plan->leadFrames;
            if( want < 0 ) want = 0;
            lastSeq_ = p.seq;
        } else if( !havePos_ ) {
            // The device has not published yet (twSpeaker defers the start
            // until the readahead is primed). Start from the locator; the first
            // real publication that disagrees is one reposition.
            want = plan->stoppedAnchor;
        }
    } else if( reposition ) {
        want = plan->stoppedAnchor;
    }

    if( !reposition && playing ) {
        // TOLERANCE. Between two publications the clock stands still, and a
        // publication that lands between two pump blocks moves it by less than
        // a block; treating either as a jump would reposition at block rate.
        // A real seek or loop wrap is orders of magnitude larger than the lead.
        const offset_t tol   = (offset_t)block * 2;
        const offset_t drift = want - nextPos_;
        if( drift > tol || drift < -tol ) reposition = true;
    }

    if( reposition ) {
        nextPos_    = want;
        blockIndex_ = 0;
        repositions_.fetch_add( 1, std::memory_order_relaxed );
        // ONE EXPLICIT REPOSITION, THROUGH THE PLAN (design D2/D4) — never
        // through the app, which is not on this thread and does not know when a
        // block boundary is. A generator then resets, chases and pre-rolls on
        // its next render; an effect starts a fresh contiguous run.
        for( const twLiveTrackPlan &t : plan->tracks )
            for( const std::shared_ptr<audio::twPluginSlotProcessor> &s : t.inserts )
                if( s ) s->forgetContinuity();
    }

    const offset_t pos = nextPos_;

    // --- the closure, children first ---------------------------------------
    for( std::size_t i = 0; i < plan->tracks.size(); ++i )
        renderTrack( *plan, (int)i, block, pos );

    // --- publish ------------------------------------------------------------
    const twLiveTrackPlan &top = plan->tracks[(std::size_t)plan->outputTrack];
    const float *src = plan->scratch( plan->outputTrack, resultBuf_[(std::size_t)plan->outputTrack] );
    const std::size_t stride = plan->scratchStride();
    const std::uint32_t ringCh = ring_.channels();
    for( std::uint32_t c = 0; c < ringCh; ++c ) {
        const idx_t sc = ( (idx_t)c < top.channels ) ? (idx_t)c : (idx_t)( top.channels - 1 );
        const float *s = src + (std::size_t)sc * stride;
        float       *d = dest + (std::size_t)c * (std::size_t)ring_.framesPerEntry();
        std::copy( s, s + (std::size_t)block, d );
    }
    ring_.commit( (std::int64_t)pos, (std::uint32_t)block, plan->flipEpoch,
                  plan->flipEpochPrime, playing );

    nextPos_ = pos + (offset_t)block;
    ++blockIndex_;
    havePos_    = true;
    wasPlaying_ = playing;
    lastPos_.store( pos, std::memory_order_relaxed );
    blocks_.fetch_add( 1, std::memory_order_relaxed );
    return true;
}

void LiveGraphPump::renderTrack( const twLivePlan &plan, int index, length_t frames,
                                 offset_t pos )
{
    const twLiveTrackPlan &t = plan.tracks[(std::size_t)index];
    const idx_t       nch    = t.channels;
    const std::size_t stride = plan.scratchStride();

    int    cur = 0;
    float *sig = plan.scratch( index, cur );
    if( !sig ) return;

    // 1. THE SOURCE. Zeroed first so every path below is a SUM and a missing
    //    input is silence rather than the previous block's audio.
    std::fill( sig, sig + (std::size_t)nch * stride, 0.0f );
    for( idx_t c = 0; c < nch; ++c ) outPtrs_[(std::size_t)c] = sig + (std::size_t)c * stride;

    if( t.input ) {
        const std::size_t got =
            t.input->pull( outPtrs_.data(), (std::size_t)nch, (std::size_t)frames, pos );
        if( got < (std::size_t)frames )
            shortfalls_.fetch_add( 1, std::memory_order_relaxed );
    }

    // A folder's LIVE children, already rendered (the plan is topologically
    // ordered and finalize() proved it).
    for( int child : t.liveChildren ) {
        const twLiveTrackPlan &c0 = plan.tracks[(std::size_t)child];
        const float *csrc = plan.scratch( child, resultBuf_[(std::size_t)child] );
        if( !csrc ) continue;
        for( idx_t c = 0; c < nch; ++c ) {
            const idx_t sc = ( c < c0.channels ) ? c : (idx_t)( c0.channels - 1 );
            const float *s = csrc + (std::size_t)sc * stride;
            float       *d = sig + (std::size_t)c * stride;
            for( length_t i = 0; i < frames; ++i ) d[i] += s[i];
        }
    }

    // A folder's UNARMED children, out of their FROZEN ROOT PAGES, by position.
    for( std::size_t k = 0; k < t.frozenInputs.size(); ++k ) {
        if( !sumFrozenInput( index, k, t.frozenInputs[k], sig, stride, nch, frames, pos ) )
            frozenMisses_.fetch_add( 1, std::memory_order_relaxed );
    }

    // 2. THE INSERTS, block-wise, in slot order. Every call is `positional`, so
    //    the processor's own continuity check is the reposition mechanism.
    for( const std::shared_ptr<audio::twPluginSlotProcessor> &s : t.inserts ) {
        if( !s ) continue;
        const float *const *in = nullptr;
        float *dst = plan.scratch( index, 1 - cur );
        if( !dst ) break;
        for( idx_t c = 0; c < nch; ++c ) {
            inPtrs_[(std::size_t)c]  = sig + (std::size_t)c * stride;
            outPtrs_[(std::size_t)c] = dst + (std::size_t)c * stride;
        }
        in = inPtrs_.data();
        s->render( in, outPtrs_.data(), frames, pos, /*positional=*/true, plan.sampleRate );
        cur = 1 - cur;
        sig = dst;
    }

    // 3. THE FADER — the same twGainStage arithmetic a frozen page gets, over
    //    the Envelope the plan snapshotted. In place: applyGain allows it.
    for( idx_t c = 0; c < nch; ++c ) {
        float *ch = sig + (std::size_t)c * stride;
        twGainStage::applyGain( ch, ch, frames, pos, t.gain );
    }

    // 4. THE CHANNEL MAP. Empty is the identity, which is every track today, so
    //    the common case copies nothing.
    if( !t.channelMap.empty() ) {
        float *dst = plan.scratch( index, 1 - cur );
        if( dst ) {
            for( idx_t c = 0; c < nch; ++c ) {
                idx_t sc = ( c < (idx_t)t.channelMap.size() ) ? t.channelMap[(std::size_t)c] : c;
                if( sc < 0 ) sc = 0;
                if( sc > nch - 1 ) sc = (idx_t)( nch - 1 );
                const float *s = sig + (std::size_t)sc * stride;
                std::copy( s, s + (std::size_t)frames, dst + (std::size_t)c * stride );
            }
            cur = 1 - cur;
        }
    }

    resultBuf_[(std::size_t)index] = cur;
}

bool LiveGraphPump::sumFrozenInput( int trackIndex, std::size_t inputIndex,
                                    const std::shared_ptr<twComponent> &comp,
                                    float *dst, std::size_t stride, idx_t channels,
                                    length_t frames, offset_t pos )
{
    if( !comp || !dst ) return false;

    const std::size_t slot    = pageBase_[(std::size_t)trackIndex] + inputIndex;
    bool              complete = true;
    length_t          done     = 0;

    while( done < frames ) {
        const offset_t p    = pos + done;
        const offset_t page = ( p < 0 ) ? 0
                                        : ( p / (offset_t)twOutputPage::FRAME_CAPACITY )
                                              * (offset_t)twOutputPage::FRAME_CAPACITY;
        const length_t take = std::min<length_t>(
            frames - done,
            (length_t)( twOutputPage::FRAME_CAPACITY - (std::size_t)( p - page ) ) );

        // TRY-LOCK ONLY. A blocking read here would put the pump behind a
        // readahead worker holding the cache; a miss is silence for this input
        // this block, which reads as a dropout and is counted.
        std::shared_ptr<twOutputPage> pg = comp->getPageIfExists( page );
        if( pg && pg->getValidAspects() == 0 ) pg.reset();   // allocated, not frozen
        std::shared_ptr<twOutputPage> &kept = heldPages_[slot];
        if( !pg && kept && kept->startPosition == page ) {
            // Keep the previous page, exactly as twLevelProbe does: a page that
            // is momentarily locked has not stopped existing.
            pg = kept;
        }
        if( pg ) kept = pg;

        if( !pg ) {
            complete = false;
            done += take;
            continue;
        }

        const std::size_t off   = (std::size_t)( p - pg->startPosition );
        const std::size_t avail = ( pg->validFrames > off ) ? ( pg->validFrames - off ) : 0;
        const length_t    n     = std::min<length_t>( take, (length_t)avail );
        if( n < take ) complete = false;

        for( idx_t c = 0; c < channels; ++c ) {
            const float *s = pg->channelPtr( twPageClampChannel( *pg, c ) ) + off;
            float       *d = dst + (std::size_t)c * stride + (std::size_t)done;
            for( length_t i = 0; i < n; ++i ) d[i] += s[i];
        }
        done += take;
    }
    return complete;
}
