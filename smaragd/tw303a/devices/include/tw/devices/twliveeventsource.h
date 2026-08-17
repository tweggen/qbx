#ifndef _TW_LIVE_EVENT_SOURCE_H_
#define _TW_LIVE_EVENT_SOURCE_H_

#include "tw/core/twtypes.h"
#include "tw/devices/midi_in_fanout.h"
#include "tw/events/tweventsource.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace audio {

// HOST TIME -> PROJECT FRAME, for the live lane (proposal 21 L2, design D2/D4).
//
// The live event source has to answer "which frame of the block I am being
// asked about was this byte pressed at", and the answer depends on the
// transport: while PLAYING it comes from the engine clock the RT callback
// stamps (tw/playback), while STOPPED there is no clock at all and the pump is
// counting virtual blocks. tw/devices may not include tw/playback, and it
// should not want to - which is why the mapping arrives as this one virtual
// call, implemented in tw/playback by twLiveEventClock.
//
// Called ON THE PUMP THREAD, inside collect(). It must be lock-free, must not
// allocate, must not block and must not touch Qt.
class twLiveFrameClock {
public:
    virtual ~twLiveFrameClock() = default;

    // `blockStart` / `blockLen` are the block being rendered right now. An
    // implementation that has no reading of its own is expected to anchor
    // "now" at `blockStart`, which is the honest answer for a stopped
    // transport: the block being rendered is the soonest thing the performer
    // can be heard in.
    virtual offset_t frameForHostTime( std::int64_t hostNs, offset_t blockStart,
                                       length_t blockLen ) const = 0;
};

// THE LIVE EVENT SOURCE (proposal 21 L2, design D4).
//
// The SECOND event source of design D2: a twEventSource that produces nothing
// from the model at all, but drains a MIDI input ring and reports what the
// performer has just played, placed inside the block being rendered.
//
// FIVE THINGS IT DOES, IN THE ORDER THEY MATTER
//
//  1. DRAINS its own SPSC sink (MidiInFanout::Sink) - the ring nobody else
//     pops. It is the pump that pops it, from inside
//     twPluginSlotProcessor::render, so the drain is allocation-free in the
//     steady state: every vector here is sized on the main thread.
//  2. MAPS host time to a project frame through twLiveFrameClock, MINUS the
//     input latency (the port's reported latency plus the user's
//     `midi/inputOffsetMs/<port>` correction). A physical key press happened
//     EARLIER than the byte arrived, so the correction moves an event earlier.
//  3. REBASES into the block: `offset = frame - startPos`.
//  4. CLAMPS A LATE EVENT TO OFFSET 0 AND NEVER DROPS IT. This is the normal
//     case, not the exceptional one: the pump renders ahead of the RT, so a
//     byte that arrives while a block is being built is by construction older
//     than that block. Clamping is what makes the latency the ring depth plus
//     the lead rather than a whole extra block, and dropping would silently
//     eat notes. An event mapped AFTER the block is kept in `pending_` and
//     emitted by the next collect, in order.
//  5. KEEPS A HELD-NOTE TABLE (and the controller state) so the ONE chase at
//     live start can re-attack what the performer is holding. A reposition
//     resets the instrument's voices; without this a key held across a
//     transport edge goes silent under the finger.
//
// THREADING. `collect()` is the PUMP. Everything else - setLatencyFrames,
// setClock, requestChase, requestAllNotesOff - is the MAIN thread and is
// published through atomics. The mutable state collect() touches has exactly
// ONE reader by construction (a ring-draining collect cannot have two, which
// is precisely why design D2 makes this a second source instead of a member of
// the track's eventFeed()).
class twLiveEventSource : public twEventSource {
public:
    // `sink` is owned by the MidiInFanout and outlives this object; the app
    // releases it after dropping the source.
    twLiveEventSource( MidiInFanout::Sink *sink, int sampleRate );
    ~twLiveEventSource() override;

    void setClock( std::shared_ptr<const twLiveFrameClock> clock );
    // Positive = the byte arrived this many frames after the key was pressed.
    void setLatencyFrames( offset_t frames );
    void setSampleRate( int rate );

    // Re-attack whatever is held, at offset 0 of the next collect, and report
    // it in the block's chase set. The app asks for this exactly where it asks
    // the pump for a reposition (design D2's "ONE explicit reposition").
    void requestChase();
    // Release everything held, at offset 0 of the next collect. The disarm
    // flush of design D4.
    void requestAllNotesOff();

    void collect( std::int64_t startPos, std::int64_t len,
                  twEventBlock &out ) const override;

    // Diagnostics; read from the main thread.
    std::uint64_t eventsEmitted() const
    { return emitted_.load( std::memory_order_relaxed ); }
    std::uint64_t lateClamped() const
    { return clamped_.load( std::memory_order_relaxed ); }
    std::uint64_t deferred() const
    { return deferred_.load( std::memory_order_relaxed ); }
    std::size_t   heldNotes() const;

private:
    struct Held {
        int      channel = 0;
        int      key     = 0;
        int      noteId  = -1;
        double   velocity = 0.0;
        offset_t start   = 0;
    };
    struct Deferred {
        offset_t frame = 0;
        MidiInMessage msg;
    };

    // Pump thread. Turns one message into 0 or 1 events appended to `out`,
    // updating the held/controller tables. `offset` is block-relative.
    void emitOne( const MidiInMessage &m, offset_t frame, std::int64_t offset,
                  twEventBlock &out ) const;
    void fillChase( twEventBlock &out ) const;

    MidiInFanout::Sink *sink_ = nullptr;

    std::atomic<int>      rate_{ 48000 };
    std::atomic<offset_t> latency_{ 0 };
    // mutable: collect() is const (twEventSource's contract) and CONSUMES
    // these two requests - the exchange is what makes "the ONE chase" one.
    mutable std::atomic<bool> chaseReq_{ false };
    mutable std::atomic<bool> panicReq_{ false };

    // shared_ptr, swapped under a mutex on the main thread and COPIED at the
    // top of collect(): the pump must never hold a raw pointer to something
    // the main thread can replace mid-block.
    std::shared_ptr<const twLiveFrameClock> clock_;
    mutable std::mutex                      clockMutex_;

    // --- pump-thread state (one reader by construction) --------------------
    mutable std::vector<Held>     held_;
    mutable std::vector<Deferred> pending_;
    mutable std::map<std::pair<std::int16_t, std::uint32_t>, double> cc_;
    mutable std::map<std::int16_t, double>  bend_;
    mutable std::map<std::int16_t, double>  pressure_;
    mutable std::map<std::int16_t, std::int32_t> program_;
    mutable std::int32_t          nextNoteId_ = 0;

    mutable std::atomic<std::uint64_t> emitted_{ 0 };
    mutable std::atomic<std::uint64_t> clamped_{ 0 };
    mutable std::atomic<std::uint64_t> deferred_{ 0 };
};

}  // namespace audio

#endif
