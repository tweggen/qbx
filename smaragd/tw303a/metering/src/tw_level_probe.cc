#include "tw/metering/tw_level_probe.h"

#include <atomic>

void twLevelProbe::setTap( std::shared_ptr<twComponent> tap )
{
    if( tap == tap_ ) return;
    tap_ = std::move( tap );
    reset();
}

void twLevelProbe::reset()
{
    held_.reset();
    lastPos_ = 0;
    havePos_ = false;
}

std::shared_ptr<twOutputPage> twLevelProbe::resolvePage_( offset_t pageStart )
{
    // WIDTH MISMATCH IS A MISS (proposal 36 §4.5). Every rung of the ladder is
    // gated on it, because rungs 2-4 all serve pages that are deliberately
    // allowed to be STALE, and a page frozen before a project width change has a
    // different geometry — not older audio. A miss decays the meter, which is
    // exactly the right reading for a position whose audio is being re-frozen.
    const idx_t widthNow = tap_->getOutputChannels();

    // Ladder step 1 & 2 — the cached page, whether or not it is from the current
    // content epoch. A stale-but-frozen page is deliberately ACCEPTED: playback
    // is serving exactly that page (proposal 16), so rejecting it would make the
    // meter disagree with the ear precisely when an edit is being absorbed.
    std::shared_ptr<twOutputPage> page = tap_->getPageIfExists( pageStart );
    if( page && page->startPosition == pageStart ) {
        if( page->validAspects.load( std::memory_order_acquire ) != 0 &&
            twPageWidthUsable( page.get(), widthNow ) ) {
            held_ = page;
            return page;
        }
        // Step 3 — the page is a placeholder being re-frozen right now; the
        // pre-edit predecessor is what playback is serving in the meantime.
        std::shared_ptr<twOutputPage> pred = std::atomic_load( &page->stalePredecessor );
        if( pred && pred->startPosition == pageStart &&
            pred->validAspects.load( std::memory_order_acquire ) != 0 &&
            twPageWidthUsable( pred.get(), widthNow ) ) {
            held_ = pred;
            return pred;
        }
    }

    // The cache read lost its try-lock (getPageIfExists never blocks, so a
    // contended lookup looks identical to an absent page), or the placeholder
    // has no predecessor yet. The page we scanned last tick is still the best
    // evidence we have about this position — but only if it IS this position.
    if( held_ && held_->startPosition == pageStart &&
        held_->validAspects.load( std::memory_order_acquire ) != 0 &&
        twPageWidthUsable( held_.get(), widthNow ) ) {
        return held_;
    }

    // Step 4 — nothing readable. The caller decays.
    return nullptr;
}

bool twLevelProbe::advanceTo( offset_t pos, twLevelSample &out, float clipThreshold )
{
    // ONE implementation, so a scalar reading and lane 0 of an N-lane reading
    // can never diverge (window arithmetic, the page ladder and the clamps all
    // live in the set form).
    twLevelSampleSet set;
    const bool ok = advanceTo( pos, set, 1, clipThreshold );
    out = ok ? set.lane[0] : twLevelSample();
    return ok;
}

bool twLevelProbe::advanceTo( offset_t pos, twLevelSampleSet &out, int wantLanes,
                              float clipThreshold )
{
    out = twLevelSampleSet();
    if( wantLanes < 1 ) wantLanes = 1;
    if( wantLanes > twLevelSampleSet::MAX_LANES )
        wantLanes = twLevelSampleSet::MAX_LANES;

    if( !tap_ ) { ++misses_; return false; }

    // --- the window that just elapsed, [winStart, pos)
    offset_t winStart;
    if( !havePos_ || pos <= lastPos_ ) {
        // No history yet, or the position did not advance: transport stopped, a
        // seek backwards, a loop wrap. Measure a SHORT window ending at pos —
        // on a first tick we do not know how long we have been running, and
        // reporting 100 ms of pre-roll would show a level from before the
        // transport rolled.
        winStart = pos - MIN_WINDOW;
    } else if( (pos - lastPos_) > MAX_WINDOW ) {
        // The pump was starved (or the position jumped forward). Cap the work,
        // but measure the frames IMMEDIATELY BEFORE pos — those are the ones
        // about to be heard. Falling back to MIN_WINDOW here would throw away
        // information for no reason.
        winStart = pos - MAX_WINDOW;
    } else {
        winStart = lastPos_;
    }
    lastPos_ = pos;
    havePos_ = true;

    if( winStart < 0 ) winStart = 0;
    if( winStart >= pos ) { ++misses_; return false; }   // nothing elapsed

    // --- clamp the window into ONE page. A window straddling a page boundary
    // loses at most MAX_WINDOW frames of its tail, once per page (1.37 s at
    // 48 kHz), and the peak of the earlier part is still reported — not worth a
    // second page lookup on the UI thread.
    const offset_t CAP       = (offset_t)twOutputPage::FRAME_CAPACITY;
    const offset_t pageStart = twFloorAlign( winStart, CAP );
    const offset_t pageEnd   = pageStart + CAP;
    const offset_t scanEnd   = (pos < pageEnd) ? pos : pageEnd;

    std::shared_ptr<twOutputPage> page = resolvePage_( pageStart );
    if( !page ) { ++misses_; return false; }

    // --- clamp into what the page actually holds. validFrames is a plain
    // uint32_t that a freeze thread may be storing concurrently; reading it (and
    // the samples themselves) while the page is re-rendered in place is the SAME
    // accepted race the audio thread already runs, and the worst case is one
    // visually wrong meter frame. samples.size() is fixed after construction, so
    // clamping against it can never go out of bounds.
    offset_t from = winStart - pageStart;               // >= 0 by floor-align
    offset_t to   = scanEnd  - pageStart;

    const offset_t valid = (offset_t)page->validFrames;
    if( to > valid ) to = valid;
    const offset_t capacity = (offset_t)page->channelFrames();
    if( to > capacity ) to = capacity;

    if( from >= to ) { ++misses_; return false; }

    // ONE LANE PER CHANNEL OF THE PAGE IN HAND, up to what the caller asked for
    // (§4.4). Never up to the tap's declared width: this page may be a forwarded
    // or default-constructed narrower one, and reading channelPtr(1) of it would
    // be out of bounds.
    int lanes = (int) page->channels();
    if( lanes > wantLanes ) lanes = wantLanes;
    if( lanes > twLevelSampleSet::MAX_LANES ) lanes = twLevelSampleSet::MAX_LANES;
    if( lanes < 1 ) lanes = 1;                 // a page always has channel 0

    for( int c = 0; c < lanes; ++c ) {
        out.lane[c] = twScanSpan( page->channelPtr( (idx_t) c ) + (size_t)from,
                                  to - from, clipThreshold );
    }
    out.lanes = lanes;
    return true;
}
