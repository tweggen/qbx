#include "tw/playback/twlivering.h"

#include <algorithm>

namespace twlive {

twLiveMixOutcome gateEntry( const twLiveRingEntry &entry, const twLiveMixGate &gate )
{
    if( entry.startPos < 0 || entry.frames == 0 || entry.channels == 0 || !entry.data )
        return twLiveMixOutcome::NoEntry;

    // (a) POSITION. The pump rendered this block FOR a frame; if the callback is
    // delivering another one, the audio belongs somewhere else. Dropping it is
    // the only honest answer — interpolating or "close enough" would hide a
    // seek, a wrap or a stalled pump behind a plausible-sounding mix.
    if( entry.startPos != gate.wantPos ) return twLiveMixOutcome::PositionMismatch;

    // (b) THE EPOCH GATE. Without a root page there is nothing to double-count:
    // that is the STOPPED case, where out = ring (design D2).
    if( gate.haveRoot ) {
        if( entry.flipEpochPrime != 0 ) {
            // DISARM. The plan's last blocks carry the epoch the root reaches
            // once the track is back in the frozen sum. Until a page that new
            // arrives, the page in hand still LACKS the track, so the ring must
            // keep filling the hole.
            if( gate.rootEpoch >= entry.flipEpochPrime )
                return twLiveMixOutcome::EpochResummed;
        } else if( entry.flipEpoch != 0 ) {
            // ARM. Until a page at least as new as the wiring bump arrives, the
            // page in hand still CONTAINS the track; summing would double it.
            if( gate.rootEpoch < entry.flipEpoch )
                return twLiveMixOutcome::EpochNotYetFlipped;
        }
    }
    return twLiveMixOutcome::Summed;
}

twLiveMixOutcome mixRing( float *const *out, std::size_t outChannels,
                          std::size_t frames, const twLiveRingEntry &entry,
                          const twLiveMixGate &gate, twLiveMixState &st )
{
    const twLiveMixOutcome verdict = gateEntry( entry, gate );
    if( verdict != twLiveMixOutcome::Summed ) {
        // THE FADE IS NOT REWOUND on a single dropped block. A ring that has
        // been running keeps its ramp position, so one missing block reads as a
        // gap and not as a re-attack of the whole crossfade.
        return verdict;
    }
    if( !out || outChannels == 0 || frames == 0 ) return twLiveMixOutcome::NoEntry;

    const std::size_t n = std::min<std::size_t>( frames, entry.frames );

    // The crossfade (design D2): a 2-3 ms ramp on both flips. With fadeFrames 0
    // it is a plain sum, which is what every test that is not about the fade
    // wants.
    const std::uint32_t N    = st.fadeFrames;
    std::uint32_t       done = st.fadeDone;

    for( std::size_t c = 0; c < outChannels; ++c ) {
        float *dst = out[c];
        if( !dst ) continue;
        const float *src = entry.channel( c );
        if( !src ) continue;

        if( N == 0 ) {
            for( std::size_t i = 0; i < n; ++i ) dst[i] += src[i];
            continue;
        }
        std::uint32_t d = done;
        for( std::size_t i = 0; i < n; ++i ) {
            const float t = ( d >= N ) ? 1.0f : ( (float)d / (float)N );
            const float g = st.fadingOut ? ( 1.0f - t ) : t;
            dst[i] += src[i] * g;
            if( d < N ) ++d;
        }
    }
    if( N != 0 )
        st.fadeDone = (std::uint32_t)std::min<std::size_t>( (std::size_t)N,
                                                           (std::size_t)done + n );
    return twLiveMixOutcome::Summed;
}

}  // namespace twlive

// ---------------------------------------------------------------- the ring

void twLiveMixRing::reset( std::uint32_t channels, std::uint32_t framesPerEntry,
                           std::uint32_t depth )
{
    if( channels == 0 ) channels = 1;
    if( depth < 2 ) depth = 2;
    if( depth > 8 ) depth = 8;

    channels_ = channels;
    frames_   = framesPerEntry;
    depth_    = ( framesPerEntry == 0 ) ? 0 : depth;

    buf_.assign( (std::size_t)channels_ * frames_ * depth_, 0.0f );
    slots_.assign( depth_, Slot{} );
    head_.store( 0, std::memory_order_relaxed );
    tail_.store( 0, std::memory_order_relaxed );
    resetStats();
}

float *twLiveMixRing::beginWrite()
{
    if( depth_ == 0 ) return nullptr;
    const std::uint64_t h = head_.load( std::memory_order_relaxed );
    const std::uint64_t t = tail_.load( std::memory_order_acquire );
    if( h - t >= (std::uint64_t)depth_ ) {
        overruns_.fetch_add( 1, std::memory_order_relaxed );
        return nullptr;   // the RT is not draining: DROP, never block or grow
    }
    const std::size_t idx = (std::size_t)( h % depth_ );
    return buf_.data() + idx * (std::size_t)channels_ * frames_;
}

void twLiveMixRing::commit( std::int64_t startPos, std::uint32_t frames,
                            std::uint64_t flipEpoch, std::uint64_t flipEpochPrime,
                            bool playing )
{
    if( depth_ == 0 ) return;
    const std::uint64_t h = head_.load( std::memory_order_relaxed );
    const std::size_t idx = (std::size_t)( h % depth_ );
    Slot &s          = slots_[idx];
    s.startPos       = startPos;
    s.frames         = std::min( frames, frames_ );
    s.flipEpoch      = flipEpoch;
    s.flipEpochPrime = flipEpochPrime;
    s.playing        = playing;
    // The release store is what publishes the SAMPLES written above, not just
    // the slot header.
    head_.store( h + 1, std::memory_order_release );
    committed_.fetch_add( 1, std::memory_order_relaxed );
}

bool twLiveMixRing::peek( twLiveRingEntry &out ) const
{
    if( depth_ == 0 ) return false;
    const std::uint64_t t = tail_.load( std::memory_order_relaxed );
    const std::uint64_t h = head_.load( std::memory_order_acquire );
    if( t >= h ) return false;

    const std::size_t idx = (std::size_t)( t % depth_ );
    const Slot &s         = slots_[idx];
    out.startPos       = s.startPos;
    out.flipEpoch      = s.flipEpoch;
    out.flipEpochPrime = s.flipEpochPrime;
    out.frames         = s.frames;
    out.playing        = s.playing;
    out.channels       = channels_;
    out.stride         = frames_;
    out.data           = buf_.data() + idx * (std::size_t)channels_ * frames_;
    return true;
}

void twLiveMixRing::pop()
{
    if( depth_ == 0 ) return;
    const std::uint64_t t = tail_.load( std::memory_order_relaxed );
    if( t >= head_.load( std::memory_order_acquire ) ) return;
    tail_.store( t + 1, std::memory_order_release );
}

std::uint32_t twLiveMixRing::dropBefore( std::int64_t pos )
{
    std::uint32_t    dropped = 0;
    twLiveRingEntry  e;
    while( peek( e ) && e.startPos >= 0 && e.startPos < pos ) {
        pop();
        ++dropped;
        mismatches_.fetch_add( 1, std::memory_order_relaxed );
    }
    return dropped;
}

void twLiveMixRing::noteOutcome( twLiveMixOutcome o )
{
    switch( o ) {
    case twLiveMixOutcome::Summed:            summed_.fetch_add( 1, std::memory_order_relaxed ); break;
    case twLiveMixOutcome::NoEntry:           misses_.fetch_add( 1, std::memory_order_relaxed ); break;
    case twLiveMixOutcome::PositionMismatch:  mismatches_.fetch_add( 1, std::memory_order_relaxed ); break;
    case twLiveMixOutcome::EpochNotYetFlipped:
    case twLiveMixOutcome::EpochResummed:     gated_.fetch_add( 1, std::memory_order_relaxed ); break;
    }
}

void twLiveMixRing::resetStats()
{
    committed_.store( 0, std::memory_order_relaxed );
    summed_.store( 0, std::memory_order_relaxed );
    mismatches_.store( 0, std::memory_order_relaxed );
    misses_.store( 0, std::memory_order_relaxed );
    gated_.store( 0, std::memory_order_relaxed );
    overruns_.store( 0, std::memory_order_relaxed );
}
