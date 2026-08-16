
//#include <qobjectlist.h>

#include <math.h>
#include <cstring>
#include <algorithm>

#include "tw/graph/tw303aenv.h"
#include <vector>
#include "tw/mix/twtrackmix.h"
#include <vector>
#include "tw/graph/twview.h"
#include <vector>
#include "tw/pages/io_vector.h"
#include "tw/core/twlog.h"
#include <vector>

int twTrackMix::seekTo( offset_t newOffset )
{
    std::lock_guard<std::mutex> lock(mutex());
    return seekTo_nolock(newOffset);
}

// Caller must hold mutex() (inherited from twComponent)
// CRITICAL: Lock protects clips_ iteration against UI thread modifications.
// Uses base class mutex to avoid introducing a second mutex (deadlock risk).
int twTrackMix::seekTo_nolock( offset_t newOffset )
{
    playOffset_.store( newOffset, std::memory_order_relaxed );

    // Propagate seek to all clips, computing their clip-relative offsets.
    // This ensures all child components are positioned correctly before the next
    // calcOutputTo call. In continuous forward play, the clip-relative offset will
    // match what calcOutputTo would compute anyway, so seekTo becomes a no-op for
    // reader cursors already at the right position. This design cleanly separates
    // concerns: "seek when position changes, advance on consecutive chunks."
    //
    // IMPORTANT: Seek ALL clips regardless of timeline position. If we only seek
    // clips within env.getBufferSize() of the seek point, clips starting later
    // retain stale reader positions (e.g., from prior playback), causing sparse/silent
    // audio when the render/playback reaches them.
    for( const ClipEntry &clip : clips_ ) {
        if( !clip.view ) {
            TW_LOGW( "mix", "WARNING: twTrackMix::seekTo_nolock found null view" );
            continue;
        }
        offset_t startTime = clip.startTime;
        // Seek this clip to the correct clip-relative position.
        // For clips not yet started (startTime > newOffset), this yields 0 (correct).
        // For clips already playing (startTime <= newOffset), this yields the offset
        // into the clip (also correct).
        offset_t clipRelative = std::max((offset_t)0, newOffset - startTime);
        clip.view->seekTo( clipRelative );
    }

    return 0;
}

idx_t twTrackMix::getNInputs() const
{
    return 0;
}

idx_t twTrackMix::getNOutputs() const
{
    return 1;
}
 
const char *twTrackMix::getInputName( idx_t ) const
{
    return NULL;
}

const char *twTrackMix::getOutputName( idx_t ) const
{
    return "Track bus sum";
}

bool twTrackMix::isSeekable() const
{
    return true;
}

// A clip's audible extent on the track timeline. Unbounded clips
// (duration 0) reach to the end of representable time.
static twEditRange clipExtent(offset_t startTime, length_t duration)
{
    twEditRange r;
    r.start = startTime;
    r.end = duration > 0 ? startTime + (offset_t) duration
                         : (offset_t) INT64_MAX;   // unbounded; see twEditRange
    return r;
}

twEditRange twTrackMix::insertClip(const void *key, offset_t startTime, length_t duration,
                            std::function<std::shared_ptr<twComponent>()> getComponentFn,
                            std::function<twResolvedClip(offset_t)> resolveFn)
{
    if( !getComponentFn ) {
        TW_LOGE( "mix", "ERROR: twTrackMix::insertClip received null callback!" );
        return {};
    }
    // Create a stable twView wrapper for this clip. Owned by shared_ptr so that
    // a preview/freeze job that captured this view stays valid even if the clip
    // is removed while the job is still queued (the queue holds its own ref).
    // make_shared also wires up enable_shared_from_this, so twView::teardown()'s
    // shared_from_this() no longer throws bad_weak_ptr.
    //
    // Construct and init the view OUTSIDE our lock: twView's ctor/init() may
    // call back into getComponentFn and the component graph, which take their
    // own locks — doing that under mutex() risks a lock-order inversion. The
    // view is not reachable by any other thread until the push_back below, so
    // building it unlocked is safe.
    auto view = std::make_shared<twView>(env, getComponentFn, resolveFn);
    view->init();

    std::lock_guard<std::mutex> lock(mutex());
    clips_.push_back({startTime, duration, key, view, nullptr});
    // Pages rendered before this edit no longer match the timeline WITHIN
    // the new clip's extent; pages elsewhere stay valid (proposal 18
    // Phase 5 range scoping).
    twEditRange r = clipExtent(startTime, duration);
    invalidatePagesInRange_nolock(r.start, r.end);
    TW_LOGD( "mix", "twTrackMix: inserted clip at time %llu, now have %zu clips", startTime, clips_.size() );
    return r;
}

twEditRange twTrackMix::removeClip(const void *key)
{
    // Deferred-destruction bucket: matched entries are MOVED here (keeping the
    // twView / previousPage shared_ptrs alive) and only destruct when this
    // vector goes out of scope. It is declared BEFORE the lock_guard so that,
    // by reverse destruction order, the mutex is released first and the
    // ~twView() run (which tears down the component, touches async queues, and
    // may take other locks) happens with our mutex OPEN — avoiding a deadlock.
    std::vector<ClipEntry> doomed;
    int removed = 0;
    twEditRange r;
    {
        std::lock_guard<std::mutex> lock(mutex());
        auto it = clips_.begin();
        while( it != clips_.end() ) {
            // Match by the caller-supplied key. The previous component-based
            // matching was doubly broken: it compared the caller's component to
            // the twView wrapper pointer (never equal, so nothing was ever
            // removed), and even comparing underlying components would remove
            // EVERY clip of the same sample, since unbuilt readers share one
            // content component.
            if( it->key == key ) {
                twEditRange e = clipExtent(it->startTime, it->duration);
                r.unite(e.start, e.end);
                // Move the entry (and its shared_ptrs) out under the lock; the
                // actual ref-count drop / dtor happens after we unlock. If a
                // preview/freeze job still holds the view, it stays alive until
                // that job releases its ref regardless.
                doomed.push_back(std::move(*it));
                it = clips_.erase(it);
                removed++;
            } else {
                ++it;
            }
        }
        if( removed > 0 ) {
            // Only the removed clip's extent went silent; pages elsewhere are
            // still correct (proposal 18 Phase 5 range scoping).
            invalidatePagesInRange_nolock(r.start, r.end);
        }
        TW_LOGD( "mix", "twTrackMix: removed %d clip(s), now have %zu clips", removed, clips_.size() );
    }
    // lock released here; `doomed` destructs next with the mutex open.
    return r;
}

twEditRange twTrackMix::updateClip(const void *key, offset_t newStartTime, length_t newDuration)
{
    // Deferred drop of the clip's previous-page snapshot. Declared before the
    // lock_guard so it destructs AFTER the mutex is released: ~twOutputPage
    // (and the shared_ptr chain it may head) must not run under our lock.
    // Deferred drop, plural: one entry per matching key (see the loop note).
    std::vector<std::shared_ptr<twOutputPage>> doomedPages;
    twEditRange r;
    {
        std::lock_guard<std::mutex> lock(mutex());
        // EVERY entry with this key, not just the first. removeClip has always
        // looped; this used to `break`, so a second entry for one SLink* would
        // stay frozen at its old extent and keep clipping the sum there. The
        // duplicate is not hypothetical: STrack::setNBusses's initial-sync loop
        // re-inserts every existing child into every bus mixer, so growing the
        // bus count duplicates each entry in the mixers that already had it.
        // Unreachable while the sink is mono — a trap primed for stereo output.
        for( ClipEntry &clip : clips_ ) {
            if( clip.key == key ) {
                // The affected extent is the UNION of the pre- and post-edit
                // windows: material vanished from the old extent and appeared
                // in the new one. Invalidate unconditionally, even if
                // startTime/duration are unchanged: slip- or stretch-only edits
                // arrive here with the same window but changed content
                // (SCut::setWindow always emits durationChanged).
                r.unite(clip.startTime, clipExtent(clip.startTime, clip.duration).end);
                twEditRange n = clipExtent(newStartTime, newDuration);
                r.unite(n.start, n.end);
                clip.startTime = newStartTime;
                clip.duration = newDuration;
                // Restart the clip's state chain: the edit may have changed the
                // component behind the view (reader rebuild, take selection), and
                // a predecessor page from another component would restore foreign
                // DSP state. Discontinuity (reset+seek) is always correct.
                // Move (not reset) the old page out so it dies after we unlock;
                // moving leaves clip.previousPage empty, which is the intent.
                if( clip.previousPage )
                    doomedPages.push_back( std::move( clip.previousPage ) );
            }
        }
        // One invalidation over the union of every match, after the walk.
        if( !r.empty() ) invalidatePagesInRange_nolock(r.start, r.end);
    }
    // lock released here; `doomedPages` destructs next with the mutex open.
    return r;
}

void twTrackMix::invalidatePagesInRange(offset_t start, offset_t end)
{
    // Deferred drop: ~twOutputPage (and the shared_ptr chain it may head) must
    // not run under our lock — same discipline as updateClip().
    std::vector<std::shared_ptr<twOutputPage>> doomed;

    // Bumps our own content epoch and stales our own pages (we cache none, but
    // the epoch is what downstream compares against).
    twComponent::invalidatePagesInRange(start, end);
    if (end <= start) return;

    {
        std::lock_guard<std::mutex> lock(mutex());
        for( ClipEntry &clip : clips_ ) {
            twEditRange e = clipExtent(clip.startTime, clip.duration);
            const bool intersects = e.start < end && start < e.end;
            if( !intersects ) continue;
            // Restart this clip's state chain, exactly as updateClip does: a
            // predecessor page rendered against pre-edit audio would restore
            // foreign DSP state. A discontinuity (reset+seek) is always correct.
            if( clip.previousPage ) doomed.push_back( std::move( clip.previousPage ) );
            if( clip.view ) clip.view->invalidatePagesInRange(start, end);
        }
    }
    // lock released; `doomed` destructs here.
}

twEditRange twTrackMix::setClipMuted(const void *key, bool muted)
{
    twEditRange r;
    {
        std::lock_guard<std::mutex> lock(mutex());
        for( ClipEntry &clip : clips_ ) {
            if( clip.key != key ) continue;
            if( clip.muted == muted ) break;      // no change, no invalidation
            clip.muted = muted;
            // Only this entry's extent changed: its material either vanished
            // from the track sum or reappeared in it (proposal 18 Phase 5
            // range scoping, same as removeClip/insertClip).
            r = clipExtent(clip.startTime, clip.duration);
            invalidatePagesInRange_nolock(r.start, r.end);
            break;
        }
    }
    return r;
}

void twTrackMix::setTrackGain(double gainDb)
{
    std::lock_guard<std::mutex> lock(mutex());
    if( trackGainDb_ != gainDb ) {
        trackGainDb_ = gainDb;
        bumpContentEpoch();  // Gain is baked into frozen pages
    }
}

// Proposal 36 B4. A twTrackMix caches no pages of its own (freezePage allocates
// a fresh page every call and never enters it into outputPages_), so widening
// costs nothing here — but every page it has already HANDED OUT is now the wrong
// shape, including the clip entries' previousPage state chain and everything
// downstream. bumpContentEpoch() covers our own hand-outs; the render chain
// above us is the caller's (STrack::bumpRenderChainEpoch), exactly as for any
// other edit.
void twTrackMix::setChannels( idx_t n )
{
    if( n < 1 ) n = 1;
    if( channels_.exchange( (int) n, std::memory_order_acq_rel ) == (int) n ) {
        return;
    }
    // Drop the per-clip DSP state chain: a previousPage of the old width would
    // be handed to a child as its predecessor, and §4.5 would then read it as a
    // miss for the rest of the session.
    {
        std::lock_guard<std::mutex> lock( mutex() );
        for( ClipEntry &c : clips_ ) c.previousPage.reset();
    }
    bumpContentEpoch();
}

void twTrackMix::createOutputLatches()
{
    pOutputLatches_[0] = std::make_shared<twStreamingLatch>( shared_from_this(), 0, 0 );
}

void twTrackMix::setBufferSize( length_t )
{
    // NYI.
    return;
}

/**
 * Calc output to of a track mixer.
 * We scan for the given offset in the track, reading then data from the
 * objects directly into the buffer.
 * Currently we do not support mixing here, but should come.
 */
// Phase 3: IOVector-based interface (type-safe, page-backed rendering)
length_t twTrackMix::calcOutputTo( IOVector& dest, idx_t idx )
{
    // Fast path: Check if component is being torn down
    if (state_.load(std::memory_order_acquire) == ComponentState::ZOMBIE) {
        return dest.fillSilence(0, dest.length());
    }

    std::lock_guard<std::mutex> lock(mutex());

    // Allocate output buffer and read buffer on heap (avoid stack overflow in deep recursion)
    std::vector<sample_t> buffer(dest.length(), 0.0f);
    std::vector<sample_t> readBuffer(env.getBufferSize());

    offset_t startInterval = playOffset_.load( std::memory_order_relaxed );
    offset_t endInterval   = startInterval + dest.length();
    playOffset_.store( endInterval, std::memory_order_relaxed );

    // Iterate through clips and mix their output
    for( const ClipEntry &clip : clips_ ) {
        if( clip.muted ) continue;      // channel mute: not summed (setClipMuted)
        offset_t startTime = clip.startTime;
        if( startTime>=endInterval ) continue;
        offset_t endTime = startTime;
        if( clip.duration > 0 ) {
            endTime += clip.duration;
            if( startInterval>=endTime ) continue;
        }

        // This clip is affected. Add the parts into the output buffer.
        offset_t startOffset;
        if( startTime<startInterval ) {
            startOffset = startInterval-startTime;
            startTime = startInterval;
        } else {
            startOffset = 0;
        }
        if( endTime ) {
            if( endTime>endInterval ) endTime = endInterval;
        } else {
            endTime = endInterval;
        }

        if( !clip.view ) {
            TW_LOGW( "mix", "WARNING: twTrackMix::calcOutputTo found null view" );
            continue;
        }
        twComponent &cp = *clip.view;
        offset_t doRead = endTime-startTime;
        offset_t destOffset = startTime-startInterval;
        memset( readBuffer.data() + destOffset, 0, sizeof( sample_t ) * doRead );

        // Get actual amount produced
        length_t actuallyGot = cp.calcOutputTo( readBuffer.data()+destOffset, doRead, idx );

        // Only mix the actual samples produced
        for( offset_t i = 0; i < actuallyGot; i++ ) {
            buffer.data()[destOffset + i] += readBuffer.data()[destOffset + i];
        }
    }

    // Apply track gain. NOT mute: mute belongs to the channel, so it is applied
    // by whoever sums THIS track (the mixer nulls our input plug; a folder track
    // skips our clip entry), never to our own output — otherwise a capture of
    // this track (an asset window) would be silence. See setClipMuted().
    double factor = pow( 10., trackGainDb_/20. );
    if( factor != 1.0 ) {
        for( offset_t i=0; i<(offset_t)dest.length(); i++ ) {
            buffer.data()[i] *= (sample_t) factor;
        }
    }

    // Copy to IOVector destination
    return dest.copyFrom(IOVector::CreateFromBuffer(buffer.data(), dest.length()), 0, dest.length());
}
// Phase 3: Freeze track output to a page
// Iterates clips that overlap [startPos, startPos+length), calls freezePage() on each
// child component, and mixes frozen outputs at correct timeline positions.
std::shared_ptr<twOutputPage> twTrackMix::freezePage(
    offset_t startPos,
    const sample_t *inputData,
    uint64_t inputOffset,
    length_t inputLength,
    int sampleRate,
    std::shared_ptr<twOutputPage> previousPage
)
{
    std::lock_guard<std::mutex> lock(mutex());
    // WIDTH (proposal 36 §7 trap 19, fixed by B4). This override allocates its
    // OWN page and therefore BYPASSES the one place a page normally learns its
    // width — twComponent::freezePage's `make_shared<twOutputPage>(
    // getOutputChannels())`. Because the render fork in freezePage_nolock is on
    // the width of the PAGE, a component that declares 4 and hands itself a
    // width-1 page would render channel 0 and publish it with NO refusal. So
    // the declared width has to be passed here, by hand, and the mixing loop
    // below has to fill every channel of what it gets.
    auto page = std::make_shared<twOutputPage>( (std::uint16_t) getOutputChannels() );
    page->startPosition = startPos;
    // Stamp with the epoch read BEFORE rendering; consumers (streaming latch,
    // downstream caches) reject pages from before the last edit.
    page->contentEpoch.store(contentEpochNow());
    // A page holds exactly FRAME_CAPACITY frames, but inputLength arrives from
    // callers that size it from CONTENT (SCut::buildCapture_ passes a whole
    // capture length), so it must be clamped before it reaches any buffer
    // arithmetic. inputLength == 0 means "no input data supplied", never
    // "render nothing" — RenderSession pulls the graph root with 0 and expects
    // a full page — so it maps to a full page here.
    const length_t pageLength = std::min<length_t>(
        inputLength ? inputLength : (length_t) twOutputPage::FRAME_CAPACITY,
        (length_t) twOutputPage::FRAME_CAPACITY);
    freezePage_nolock(page, startPos, pageLength, sampleRate, previousPage);
    // Snapshot track state for restoration at next page boundary (mutex already held)
    page->internalState = captureInternalState_nolock();
    return page;
}

// Proposal 19 dataflow stage 2 — planner override (see header doc). Mirrors
// freezePage_nolock's clip-overlap walk EXACTLY (same clipEnd/childPos
// arithmetic), but resolves structurally instead of rendering.
twPagePlan twTrackMix::planPage( offset_t pageStart )
{
    twPagePlan plan;
    plan.component = shared_from_this();
    plan.pageStart = pageStart;
    plan.epoch     = contentEpochNow();

    const offset_t endPos = pageStart + twOutputPage::FRAME_CAPACITY;

    std::lock_guard<std::mutex> lock(mutex());
    for( const ClipEntry &clip : clips_ ) {
        offset_t clipEnd = clip.startTime;
        if( clip.duration > 0 ) {
            clipEnd += clip.duration;
        } else {
            clipEnd = endPos;      // unbounded clip: extends to page end
        }
        if( clip.startTime >= endPos || clipEnd <= pageStart ) {
            continue;              // no overlap with this page
        }
        if( !clip.view ) continue;
        // Channel mute: not summed, so do not make the scheduler demand pages
        // nothing will consume (setClipMuted).
        if( clip.muted ) continue;

        offset_t childPos = (pageStart >= clip.startTime)
                            ? (pageStart - clip.startTime)
                            : 0;
        // The SAME single resolution the render's view->freezePage performs
        // (Inv-1): component identity + mapped position from one snapshot.
        // mappedPos may be NEGATIVE (clip anchored before its data) and is
        // carried as such — the old cast to unsigned wrapped it here, which is
        // what made that page render silent (proposal 23).
        twResolvedClip r = clip.view->resolve( childPos );
        if( !r.component ) continue;
        plan.deps.push_back( twPageDep{ r.component, r.mappedPos } );
    }
    return plan;
}

// Caller must hold mutex()
length_t twTrackMix::freezePage_nolock(
    std::shared_ptr<twOutputPage> page,
    offset_t startPos,
    length_t length,
    int sampleRate,
    std::shared_ptr<twOutputPage> previousPage
)
{
    // twTrackMix overrides the WHOLE freeze rather than the base
    // freezePage_nolock, so the base's freeze-window marker never runs for it —
    // mark it here, or an external seek() of a track mix mid-freeze would go
    // undetected (see twComponent::seek).
    FreezeInFlight inFlight( *this );

    // Restore track-level state from previous page snapshot (mutex already held)
    if (previousPage) {
        restoreInternalState_nolock(previousPage->internalState);
    }

    // Second line of defence (freezePage clamps too): every use of `length`
    // below writes into or bounds a fixed-capacity page buffer, and endPos
    // drives the clip-overlap walk — an over-long length both overruns the fill
    // and drags every clip in the track into this page's mix.
    if (length < 0) length = 0;
    length = std::min<length_t>(length, (length_t) page->channelFrames());

    // Initialize output buffer to silence. Per channel: at width 1 this is the
    // single fill it always was, and at width > 1 (B4) it is still the right
    // thing rather than a hard-coded channel 0 that would leave the other
    // channels holding whatever the allocation left there.
    for (idx_t c = 0; c < (idx_t) page->channels(); ++c) {
        sample_t *dst = page->channelPtr(c);
        std::fill(dst, dst + length, 0.0f);
    }

    offset_t endPos = startPos + length;

    // Iterate through clips that overlap [startPos, endPos)
    for( ClipEntry &clip : clips_ ) {  // Non-const to update previousPage
        offset_t clipEnd = clip.startTime;
        if( clip.duration > 0 ) {
            clipEnd += clip.duration;
        } else {
            // Unbounded clip: treat as extending to page end
            clipEnd = endPos;
        }

        // Skip clips that don't overlap this page
        if( clip.startTime >= endPos || clipEnd <= startPos ) {
            continue;
        }

        // Freeze the child component's output for this range
        if( !clip.view ) {
            TW_LOGW( "mix", "WARNING: twTrackMix::freezePage_nolock found null view" );
            continue;
        }
        // Channel mute: not summed into this track (setClipMuted). Matches the
        // planPage skip, so plan and render agree on the deps.
        if( clip.muted ) continue;

        // Child position: what frame offset in the child corresponds to startPos?
        offset_t childPos = (startPos >= clip.startTime)
                            ? (startPos - clip.startTime)
                            : 0;

        // Pass the clip's previous page so state carries forward across page boundaries
        // If this is the first page for this clip, previousPage will be nullptr (correct)
        auto childPage = clip.view->freezePage(
            childPos,
            nullptr,  // Track outputs don't consume input
            0,
            length,
            sampleRate,
            clip.previousPage  // Resume from previous page's state snapshot
        );

        if( !childPage || childPage->validFrames == 0 ) {
            continue;
        }

        // Save this page as the clip's previous page for the next page boundary
        clip.previousPage = childPage;

        // Mix child's frozen output into track output at correct timeline position
        offset_t destOffset = (clip.startTime >= startPos)
                              ? (clip.startTime - startPos)
                              : 0;

        // Clamp to the clip's end: a frozen page always carries a full page of
        // source material, so without this the last page of a clip would bleed
        // audio past the clip boundary into the track sum.
        offset_t mixStart = startPos + destOffset;
        offset_t framesToMix = std::min<offset_t>((offset_t) childPage->validFrames,
                                                  clipEnd > mixStart ? clipEnd - mixStart : 0);

        // Type-safe mixing using IOVector (bounds-checked, zero-copy), ONE
        // CHANNEL AT A TIME (proposal 36 §4.6: IOVector stays a single-channel
        // view and wide mixing is a loop over channels of the same page pair).
        //
        // The source channel is CLAMPED, never assumed: a clip's page carries
        // the width of ITS source (a mono file freezes width 1, a stereo file
        // width 2, and nothing threads the track's width into a reader). §4.4 —
        // "mono plays on every channel" — is what makes a mono clip audible on
        // every channel of a wide track, and it is exactly the behaviour the
        // retired N-parallel-mixer arrangement produced by rendering the same
        // channel-0 page into each of its N buses.
        const idx_t nCh = (idx_t) page->channels();
        for( idx_t c = 0; c < nCh; ++c ) {
            IOVector childVec = IOVector::CreateForPageOutput(
                childPage, twPageClampChannel( *childPage, c ) );
            IOVector outputVec(page, 0, length, c);
            outputVec.mixFrom(childVec, destOffset, (length_t) framesToMix);
        }
    }

    // Apply track gain, not mute (same as calcOutputTo_nolock — see there).
    double factor = pow( 10., trackGainDb_/20. );
    if( factor != 1.0 ) {
        const size_t n = std::min<size_t>( (size_t) length, page->channelFrames() );
        for( idx_t c = 0; c < (idx_t) page->channels(); ++c ) {
            sample_t *dst = page->channelPtr( c );
            for( size_t i = 0; i < n; ++i ) {
                dst[i] *= (sample_t) factor;
            }
        }
    }

    page->validFrames = std::min((uint32_t)length, (uint32_t)page->channelFrames());
    page->validAspects = twAspectPlayback;  // We've computed playback data
    return page->validFrames;
}

// Phase 3: Preview page freezing at lower resolution
// Delegates to base class freezePreviewPage() which calls freezePage() at preview rate
std::shared_ptr<twOutputPage> twTrackMix::freezePreviewPage(
    offset_t startPos,
    length_t length,
    int previewSampleRate,
    int fullSampleRate,
    std::shared_ptr<twOutputPage> previousPage
)
{
    // Use base class implementation: calls freezePage() at preview resolution
    // Non-blocking: returns previousPage if new page not ready
    return twComponent::freezePreviewPage(startPos, length, previewSampleRate, fullSampleRate, previousPage);
}

// Helper: capture state without acquiring mutex (caller must hold mutex)
std::any twTrackMix::captureInternalState_nolock() const
{
    return std::any(TrackInternalState{
        playOffset_.load(std::memory_order_relaxed)
    });
}

// Capture track-level state for snapshot-based rendering across page boundaries
std::any twTrackMix::captureInternalState() const
{
    std::lock_guard<std::mutex> lock(mutex());
    return captureInternalState_nolock();
}

// Helper: restore state without acquiring mutex (caller must hold mutex)
void twTrackMix::restoreInternalState_nolock(const std::any& state)
{
    try {
        auto s = std::any_cast<const TrackInternalState&>(state);
        playOffset_.store(s.playOffset, std::memory_order_relaxed);
    } catch (const std::bad_any_cast&) {
        TW_LOGD( "mix", "twTrackMix::restoreInternalState: state format mismatch" );
    }
}

// Restore track-level state from previous page snapshot
void twTrackMix::restoreInternalState(const std::any& state)
{
    std::lock_guard<std::mutex> lock(mutex());
    restoreInternalState_nolock(state);
}

twTrackMix::~twTrackMix()
{
}

twTrackMix::twTrackMix( tw303aEnvironment &env )
    : twComponent( env ),
      playOffset_( 0 ),
      trackGainDb_( 0.0 )
{
}


void twTrackMix::reset()
{
    // Reset play offset to zero
    playOffset_ = 0;
}

void twTrackMix::teardown()
{
    state_.store(ComponentState::ZOMBIE, std::memory_order_release);

    if (auto parent = parentComponent_.lock()) {
        if (myInputIndex_ >= 0) {
            parent->removeInput(myInputIndex_);
        }
    }

    std::vector<std::shared_ptr<twComponent> > depsCopy;
    {
        std::lock_guard<std::mutex> lock(mutex());
        depsCopy = dependents_;
    }
    for (auto dep : depsCopy) {
        if (dep) dep->onDependencyTeardown(shared_from_this());
    }

    // Snapshot clips and tear them down
    std::vector<std::shared_ptr<twView>> clipsCopy;
    {
        std::lock_guard<std::mutex> lock(mutex());
        for (const ClipEntry &clip : clips_) {
            if (clip.view) {
                clipsCopy.push_back(clip.view);
            }
        }
        clips_.clear();  // Prevent audio thread from iterating stale list
    }

    // Tear down all clips
    for (auto clip : clipsCopy) {
        if (clip) clip->teardown();
    }
}
