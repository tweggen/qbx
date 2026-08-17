#include "tw/playback/twlivering.h"

#include <algorithm>

namespace twlive {

twLiveMixOutcome gateEpoch( const twLiveRingEntry &entry, const twLiveMixGate &gate )
{
    if( entry.startPos < 0 || entry.frames == 0 || entry.channels == 0 || !entry.data )
        return twLiveMixOutcome::NoEntry;

    // Without a root page there is nothing to double-count: that is the
    // STOPPED case, where out = ring (design D2).
    if( !gate.haveRoot ) return twLiveMixOutcome::Summed;

    if( entry.flipEpochPrime != 0 ) {
        // DISARM. The last blocks of the plan carry the epoch the root reaches
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
    return twLiveMixOutcome::Summed;
}

twLiveMixOutcome gateEntry( const twLiveRingEntry &entry, const twLiveMixGate &gate )
{
    const twLiveMixOutcome epochs = gateEpoch( entry, gate );
    if( epochs == twLiveMixOutcome::NoEntry ) return epochs;

    // POSITION, as an EXACT claim. This is the ONE-ENTRY primitive and the
    // strictest reading there is; the stream consumer deliberately does not use
    // it, because the RT block grid and the pump block grid are different grids
    // by construction (twlivering.h, and the variable WASAPI block of F6).
    if( entry.startPos != gate.wantPos ) return twLiveMixOutcome::PositionMismatch;
    return epochs;
}

namespace {

// Sum `n` frames of `entry`, starting `srcOff` frames into it, into `out` at
// `dstOff`, advancing the crossfade. The ONE place samples are added: both the
// one-entry primitive and the stream consumer go through it, so the fade and
// the channel clamp cannot drift apart.
void sumSpan( float *const *out, std::size_t outChannels, std::size_t dstOff,
              const twLiveRingEntry &entry, std::size_t srcOff, std::size_t n,
              twLiveMixState &st )
{
    if( n == 0 ) return;
    const std::uint32_t N = st.fadeFrames;

    for( std::size_t c = 0; c < outChannels; ++c ) {
        float *dst = out[c];
        if( !dst ) continue;
        const float *src = entry.channel( c );
        if( !src ) continue;
        dst += dstOff;
        src += srcOff;

        if( N == 0 ) {
            for( std::size_t i = 0; i < n; ++i ) dst[i] += src[i];
            continue;
        }
        std::uint32_t d = st.fadeDone;
        for( std::size_t i = 0; i < n; ++i ) {
            const float t = ( d >= N ) ? 1.0f : ( (float)d / (float)N );
            const float g = st.fadingOut ? ( 1.0f - t ) : t;
            dst[i] += src[i] * g;
            if( d < N ) ++d;
        }
    }
    if( N != 0 )
        st.fadeDone = (std::uint32_t)std::min<std::size_t>(
            (std::size_t)N, (std::size_t)st.fadeDone + n );
}

}  // namespace

twLiveMixOutcome mixRing( float *const *out, std::size_t outChannels,
                          std::size_t frames, const twLiveRingEntry &entry,
                          const twLiveMixGate &gate, twLiveMixState &st )
{
    const twLiveMixOutcome verdict = gateEntry( entry, gate );
    if( verdict != twLiveMixOutcome::Summed ) {
        // THE FADE IS NOT REWOUND on a single refused block. A ring that has
        // been running keeps its ramp position, so one missing block reads as a
        // gap and not as a re-attack of the whole crossfade.
        return verdict;
    }
    if( !out || outChannels == 0 || frames == 0 ) return twLiveMixOutcome::NoEntry;

    sumSpan( out, outChannels, 0, entry, 0,
             std::min<std::size_t>( frames, entry.frames ), st );
    return twLiveMixOutcome::Summed;
}

void mixStream( float *const *out, std::size_t outChannels, std::size_t frames,
                std::int64_t wantPos, const twLiveMixGate &gate,
                twLiveMixRing &ring, twLiveMixReader &reader,
                twLiveMixState &st, twLiveStreamStats &stats )
{
    stats = twLiveStreamStats{};
    if( !out || outChannels == 0 || frames == 0 ) return;

    std::size_t done = 0;
    // A bound on the loop, not a policy: every iteration either advances `done`
    // or consumes/pops an entry, and the ring is finitely deep, so this can be
    // reached only by a corrupted ring. It exists because this runs on the RT
    // thread, where an unbounded loop is a hang rather than a bug report.
    const int kMaxSteps = 64 + 4 * (int)ring.depth();

    for( int step = 0; done < frames && step < kMaxSteps; ++step ) {
        twLiveRingEntry e;
        if( !ring.peek( e ) ) {
            // STARVED. The rest of the block stays whatever the caller left
            // there: silence, or the frozen lane alone. Never a wait.
            ++stats.starved;
            stats.framesSilent += (std::uint32_t)( frames - done );
            break;
        }
        if( e.startPos < 0 || e.frames == 0 || !e.data ) {
            ring.pop(); reader.rewind(); continue;
        }
        if( e.runId != ring.currentRun() ) {
            // AN ABANDONED RUN. The pump repositioned; this entry describes a
            // timeline that no longer exists and the RT will never ask for it.
            // Dropping it here is what keeps the keep-the-future rule from
            // filling the ring and starving the producer (see the header).
            ring.pop(); reader.rewind(); ++stats.dropped; continue;
        }
        if( reader.cursor >= e.frames ) { ring.pop(); reader.rewind(); continue; }

        const std::int64_t entryPos  = e.startPos + (std::int64_t)reader.cursor;
        const std::size_t  entryLeft = (std::size_t)( e.frames - reader.cursor );
        const std::int64_t want      = wantPos + (std::int64_t)done;

        if( entryPos + (std::int64_t)entryLeft <= want ) {
            // WHOLLY BEHIND: the RT has moved past it (a seek forward, or a
            // callback that ran late). Drop it and look at the next one.
            ring.pop();
            reader.rewind();
            ++stats.dropped;
            continue;
        }
        if( entryPos > want ) {
            // THE FUTURE. Emit silence for the gap and KEEP the entry: popping
            // it would throw away audio the very next callback needs.
            const std::size_t gap = std::min<std::size_t>(
                frames - done, (std::size_t)( entryPos - want ) );
            done += gap;
            ++stats.notYet;
            stats.framesSilent += (std::uint32_t)gap;
            continue;
        }
        if( entryPos < want ) {
            // Partially behind: skip the head of it and re-decide.
            reader.cursor += (std::uint32_t)( want - entryPos );
            continue;
        }

        // OVERLAP. The verdict is per ENTRY, so a partially consumed entry
        // keeps the one its epochs gave it.
        const std::size_t      n = std::min<std::size_t>( frames - done, entryLeft );
        const twLiveMixOutcome v = gateEpoch( e, gate );
        if( v == twLiveMixOutcome::Summed ) {
            sumSpan( out, outChannels, done, e, reader.cursor, n, st );
            ++stats.summed;
            stats.framesSummed += (std::uint32_t)n;
        } else {
            ++stats.gated;
            stats.framesSilent += (std::uint32_t)n;
        }
        reader.cursor += (std::uint32_t)n;
        done          += n;
        if( reader.cursor >= e.frames ) { ring.pop(); reader.rewind(); }
    }
    ring.noteStream( stats );
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
    run_.store( 0, std::memory_order_relaxed );
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
    s.runId          = run_.load( std::memory_order_relaxed );
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
    out.runId          = s.runId;
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

std::uint32_t twLiveMixRing::pending() const
{
    const std::uint64_t t = tail_.load( std::memory_order_relaxed );
    const std::uint64_t h = head_.load( std::memory_order_acquire );
    return (std::uint32_t)( h - t );
}

void twLiveMixRing::noteStream( const twLiveStreamStats &s )
{
    if( s.summed )  summed_.fetch_add( s.summed, std::memory_order_relaxed );
    if( s.dropped ) dropped_.fetch_add( s.dropped, std::memory_order_relaxed );
    if( s.notYet )  notYet_.fetch_add( s.notYet, std::memory_order_relaxed );
    if( s.gated )   gated_.fetch_add( s.gated, std::memory_order_relaxed );
    if( s.starved ) misses_.fetch_add( s.starved, std::memory_order_relaxed );
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
    dropped_.store( 0, std::memory_order_relaxed );
    notYet_.store( 0, std::memory_order_relaxed );
}
