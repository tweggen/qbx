#ifndef _TW_LIVE_EVENT_CLOCK_H_
#define _TW_LIVE_EVENT_CLOCK_H_

#include "tw/devices/twliveeventsource.h"
#include "tw/playback/twliveclock.h"

#include <atomic>
#include <cstdint>

// THE LIVE EVENT CLOCK (proposal 21 L2, design D2/D4).
//
// The one implementation of `audio::twLiveFrameClock`: it answers "which
// project frame was this host time" for the live event source, and the answer
// is a different one in each half of design D2's transport table.
//
//   PLAYING  the ENGINE CLOCK is authoritative. `twEngineClock::read()` gives
//            {deliveredFrame, hostNs} - the frame the device is being handed
//            RIGHT NOW, stamped by the RT callback - so
//
//                frame(T) = deliveredFrame + (T - hostNs) * rate / 1e9
//
//            A byte that arrived a moment ago therefore lands near the frame
//            the ear is at, which is BEHIND the block the pump is building
//            (the pump runs `lead` frames ahead). The source clamps that to
//            offset 0, which is exactly right: the soonest the performer can
//            be heard is the block being written.
//
//   STOPPED  there is no engine clock at all - the pump is counting virtual
//            blocks from the locator - so "now" is anchored at the BLOCK being
//            rendered. Same conclusion, arrived at without pretending to a
//            reading that does not exist.
//
// It is a class in tw/playback rather than app code because it is pure engine
// arithmetic over an engine atomic, and because the pump thread must be able to
// call it with no Qt anywhere near. `setPlaying()` is the MAIN thread, at every
// plan publication; `frameForHostTime()` is the PUMP and is lock-free.
class twLiveEventClock : public audio::twLiveFrameClock {
public:
    twLiveEventClock( const twEngineClock &clock, int sampleRate )
        : clock_( clock )
    {
        rate_.store( sampleRate > 0 ? sampleRate : 48000, std::memory_order_relaxed );
    }

    void setPlaying( bool playing )
    { playing_.store( playing, std::memory_order_release ); }
    void setSampleRate( int rate )
    { if( rate > 0 ) rate_.store( rate, std::memory_order_relaxed ); }

    offset_t frameForHostTime( std::int64_t hostNs, offset_t blockStart,
                               length_t blockLen ) const override
    {
        const std::int64_t rate = (std::int64_t) rate_.load( std::memory_order_relaxed );
        if( playing_.load( std::memory_order_acquire ) ) {
            const twEnginePosition p = clock_.read();
            if( p.valid() ) {
                // Nanoseconds x frames-per-second, divided once. int64 holds a
                // 292-year span at nanosecond resolution, and the delta here is
                // milliseconds; the product is bounded by rate * delta and
                // cannot overflow for any real reading.
                const std::int64_t dNs = hostNs - p.hostNs;
                const std::int64_t dFr = ( dNs * rate ) / 1'000'000'000LL;
                return (offset_t)( p.deliveredFrame + dFr );
            }
            // Playing but nothing published yet (twSpeaker defers the device
            // start until the readahead is primed - the same hole SMidiOutPump's
            // anchor has). Fall through to the stopped rule rather than invent
            // a position.
        }
        (void) blockLen;
        return blockStart;
    }

private:
    const twEngineClock &clock_;
    std::atomic<bool>    playing_{ false };
    std::atomic<int>     rate_{ 48000 };
};

#endif  // _TW_LIVE_EVENT_CLOCK_H_
