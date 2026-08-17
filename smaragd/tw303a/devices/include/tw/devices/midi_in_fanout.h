#ifndef _TW_MIDI_IN_FANOUT_H_
#define _TW_MIDI_IN_FANOUT_H_

#include "tw/devices/midi_in_ring.h"
#include "tw/devices/midi_input.h"

#include <atomic>
#include <cstdint>

namespace audio {

class MidiOutScheduler;

// THE FAN-OUT AT THE MIDI INPUT CALLBACK (proposal 21 L2, design D8).
//
// One MidiInput port delivers on ONE device thread, and several things in the
// app want what it delivers: the live lane (per consuming instrument), the MIDI
// recorder (L4), and MIDI-thru. The design's rule is that the device thread
// writes ONE RING PER CONSUMER rather than everybody sharing one - SPSC stays
// SPSC, and a slow consumer can only starve itself.
//
// LIFETIME IS WHY THE SINKS ARE OWNED HERE AND NEVER FREED. A consumer
// registering its own ring would have to unregister it and then prove the
// device thread was not inside a push() on it - a lifetime problem with no
// lock-free answer. Instead the fanout owns a FIXED array of sinks for its
// whole life; acquiring one flips an atomic flag, releasing it flips it back,
// and the device thread only ever touches memory the fanout still owns. A
// released sink is inert (its flag is false) and reusable.
//
// THRU has no ring of its own here: the bytes go straight into the IMMEDIATE
// ring MidiOutScheduler grew for exactly this (design D8) - never enqueue(),
// which is main-thread single-producer (devices inv. 11). One thru target per
// fanout: the immediate ring is SPSC too, so its producer must be one device
// thread. A second request is REFUSED and logged rather than silently racing
// the first.
//
// THREADING. onMessage() is the device thread: no allocation, no lock, no Qt.
// acquire() / release() / setThru() are the MAIN thread.
class MidiInFanout {
public:
    // Enough for one recorder plus a handful of live instruments. A project
    // with more armed MIDI consumers than this on ONE port gets a logged
    // refusal, not a silent drop.
    static constexpr int kMaxSinks = 8;

    // One consumer's private view of the port.
    struct Sink {
        MidiInRing                 ring{ 1024 };
        std::atomic<bool>          active{ false };
        // Bit c set == channel c passes. 0xFFFF is "any". A channel-voice
        // message is filtered here, on the DEVICE thread, so a consumer's ring
        // never fills with traffic it would throw away.
        std::atomic<std::uint16_t> channelMask{ 0xFFFF };
        std::atomic<std::uint64_t> pushed{ 0 };
        std::atomic<std::uint64_t> dropped{ 0 };

        bool pop( MidiInMessage &m ) { return ring.pop( m ); }
    };

    MidiInFanout() = default;
    MidiInFanout( const MidiInFanout & )            = delete;
    MidiInFanout &operator=( const MidiInFanout & ) = delete;

    // Main thread. Null when every sink is in use.
    Sink *acquire( std::uint16_t channelMask, const char *label );
    void  release( Sink *s );

    // Main thread. `sched` must outlive the fanout's use of it - every
    // MidiOutScheduler in this app is kept for the process by SMidiOutPump, so
    // that holds by construction. `channelOverride` >= 0 rewrites the channel
    // nibble of every channel-voice message; -1 sends it as played. Returns
    // false when a DIFFERENT thru target is already set.
    bool setThru( MidiOutScheduler *sched, int channelOverride );
    void clearThru();
    bool hasThru() const { return thru_.load( std::memory_order_acquire ) != nullptr; }

    // THE DEVICE THREAD. Bind it with callback() and hand that to
    // MidiInput::setCallback().
    void onMessage( const std::uint8_t *bytes, std::size_t n,
                    std::int64_t hostTimeNs );
    MidiInputCallback callback();

    std::uint64_t messages() const
    { return messages_.load( std::memory_order_relaxed ); }
    std::uint64_t thruSent() const
    { return thruSent_.load( std::memory_order_relaxed ); }
    std::uint64_t thruDropped() const
    { return thruDropped_.load( std::memory_order_relaxed ); }

private:
    Sink sinks_[kMaxSinks];

    std::atomic<MidiOutScheduler *> thru_{ nullptr };
    std::atomic<int>                thruChannel_{ -1 };

    std::atomic<std::uint64_t> messages_{ 0 };
    std::atomic<std::uint64_t> thruSent_{ 0 };
    std::atomic<std::uint64_t> thruDropped_{ 0 };
};

}  // namespace audio

#endif
