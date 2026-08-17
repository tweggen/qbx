#ifndef _TW_LIVE_CLOCK_H_
#define _TW_LIVE_CLOCK_H_

#include <atomic>
#include <cstdint>

// THE LIVE CLOCK (proposal 21 L1a, design D2).
//
// The live pump renders every block AT A POSITION, and while the transport is
// PLAYING that position has to be the one the ear is at — otherwise a monitored
// input is mixed against arrangement audio from a different bar. The only place
// in the process that knows it is the RENDER CALLBACK, which is why this atomic
// is stamped there, beside publishPosition(), and why it is ENGINE-owned:
//
//   - `audio::PlaybackContext` is the APP-implemented services interface. Its
//     locatorPosition() is documented UI-thread and its publishPosition() is a
//     one-way store into the app's locator. Reading a position back out of the
//     app would put a UI-thread value on the pump's critical path and invert
//     the dependency the modularization (proposal 14) exists to keep one-way.
//   - `SApplication` is not reachable from tw/ at all.
//
// THE THREE FIELDS, AND WHY DELIVERED IS NOT PUBLISHED.
//
//   seq            monotone publication count. A pump that sees the same seq
//                  twice knows the callback has not run since — which is how it
//                  distinguishes "the playhead is not moving" from "the
//                  playhead moved back to the same number".
//   nextFrame      the PROJECT frame the RT will pull NEXT -- exactly the
//                  position the callback publishes. This is what the PUMP
//                  paces on (review fix 2): it keeps [nextFrame, nextFrame +
//                  lead) covered, so "how far ahead am I" is a question about
//                  the frames the RT is about to ask for rather than about
//                  block counts. Keeping it separate from deliveredFrame is
//                  what lets MIDI-out and metering go on reading the frame
//                  being HEARD while the pump reads the frame being WANTED;
//                  the two differ by one device buffer and conflating them
//                  would put the live lane a buffer behind the arrangement.
//   deliveredFrame the PROJECT frame the device is being handed right now:
//                  `engine->currentPosition() - bufferFrames`. twSpeaker
//                  publishes the position AFTER the pull, so the published
//                  value means "everything up to P has been handed over"; the
//                  frame actually going out is one device buffer earlier. This
//                  is exactly SMidiOutPump's publish-lag correction (proposal
//                  37 D6), applied once, here, so every consumer of the live
//                  clock shares one definition instead of re-deriving it.
//   hostNs         MidiOutScheduler::hostNowNs() at the stamp — the same steady
//                  clock CaptureBackend stamps its block log with and the MIDI
//                  bridge maps host time onto, so a live event's arrival time
//                  and this anchor are in one domain.
//
// It is deliberately NOT latency-compensated: `outputLatencyFrames` says when
// the frame will be HEARD, which is what a MIDI-out send and a meter want. The
// pump wants the frame the DEVICE IS TAKING, plus its own lead. Mixing the two
// would double-count.
//
// Threading: ONE writer (the RT callback), any number of readers. A seqlock
// rather than a mutex — the callback may never block — and rather than three
// independent atomics, which would let a reader pair a new frame with an old
// host time and compute a bogus rate. Readers retry a BOUNDED number of times
// and then report "no reading" instead of spinning: the pump is realtime too.

struct twEnginePosition {
    std::uint64_t seq            = 0;    // 0 == nothing published yet
    std::int64_t  deliveredFrame = 0;
    std::int64_t  nextFrame      = 0;
    std::int64_t  hostNs         = 0;

    bool valid() const { return seq != 0; }
};

class twEngineClock {
public:
    // RT callback only. Cheap: three relaxed stores between two releases.
    void stamp( std::int64_t deliveredFrame, std::int64_t nextFrame,
                std::int64_t hostNs )
    {
        const std::uint64_t s = guard_.load( std::memory_order_relaxed );
        guard_.store( s + 1, std::memory_order_release );          // odd: writing
        std::atomic_thread_fence( std::memory_order_release );
        frame_.store( deliveredFrame, std::memory_order_relaxed );
        next_.store( nextFrame, std::memory_order_relaxed );
        host_.store( hostNs, std::memory_order_relaxed );
        // A stamp is a live reading by definition, so it also UNDOES an
        // invalidate(): a new device session needs no separate revalidation.
        valid_.store( true, std::memory_order_relaxed );
        std::atomic_thread_fence( std::memory_order_release );
        guard_.store( s + 2, std::memory_order_release );          // even: stable
    }

    // The transport stopped, the device closed, or the engine went away: what
    // was published describes a playhead that no longer exists. A pump that
    // reads an invalid clock falls back to its virtual counter (design D2).
    void invalidate()
    {
        const std::uint64_t s = guard_.load( std::memory_order_relaxed );
        guard_.store( s + 1, std::memory_order_release );
        valid_.store( false, std::memory_order_relaxed );
        guard_.store( s + 2, std::memory_order_release );
    }

    void revalidate() { valid_.store( true, std::memory_order_release ); }

    // Any thread. Returns an invalid reading (seq == 0) when nothing has been
    // published, when the clock has been invalidated, or when four consecutive
    // attempts were torn by the writer.
    twEnginePosition read() const
    {
        twEnginePosition out;
        for( int attempt = 0; attempt < 4; ++attempt ) {
            const std::uint64_t g0 = guard_.load( std::memory_order_acquire );
            if( g0 & 1u ) continue;                       // mid-write
            std::atomic_thread_fence( std::memory_order_acquire );
            const std::int64_t f = frame_.load( std::memory_order_relaxed );
            const std::int64_t x = next_.load( std::memory_order_relaxed );
            const std::int64_t h = host_.load( std::memory_order_relaxed );
            const bool         v = valid_.load( std::memory_order_relaxed );
            std::atomic_thread_fence( std::memory_order_acquire );
            if( guard_.load( std::memory_order_acquire ) != g0 ) continue;
            if( !v || g0 == 0 ) return out;               // never stamped / invalidated
            out.seq            = g0 / 2;                  // publications, not guard ticks
            out.deliveredFrame = f;
            out.nextFrame      = x;
            out.hostNs         = h;
            return out;
        }
        return out;
    }

private:
    std::atomic<std::uint64_t> guard_{ 0 };   // even == stable, odd == writing
    std::atomic<std::int64_t>  frame_{ 0 };
    std::atomic<std::int64_t>  next_{ 0 };
    std::atomic<std::int64_t>  host_{ 0 };
    std::atomic<bool>          valid_{ true };
};

#endif  // _TW_LIVE_CLOCK_H_
