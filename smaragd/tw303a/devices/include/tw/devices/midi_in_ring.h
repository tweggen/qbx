#ifndef _TW_MIDI_IN_RING_H_
#define _TW_MIDI_IN_RING_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace audio {

// ONE MIDI message as it left the device, with the host time it arrived at
// (MidiOutScheduler::hostNowNs() domain — the same steady clock the audio
// capture backend stamps its block log with, which is what makes an input
// event's arrival and a rendered frame comparable at all).
//
// The bytes are INLINE and fixed-length on purpose: a ring that allocates is
// not a ring, and the producer is a device callback that may not touch the
// heap. A message longer than kMaxMessageBytes (a sysex dump) is REFUSED and
// counted rather than truncated — the same rule MidiOutScheduler's own ring
// states, and for the same reason (a truncated sysex is worse than a missing
// one). Sysex over the live lane is proposal 21 §13 / 37 P9 work.
struct MidiInMessage {
    std::int64_t hostTimeNs = 0;
    std::uint8_t size       = 0;
    std::uint8_t bytes[16]  = {};

    static constexpr std::size_t kMaxMessageBytes = 16;

    int  status()  const { return size ? ( bytes[0] & 0xF0 ) : 0; }
    int  channel() const { return size ? ( bytes[0] & 0x0F ) : -1; }
    bool isChannelVoice() const
    { return size >= 2 && bytes[0] >= 0x80 && bytes[0] < 0xF0; }
};

// A single-producer / single-consumer ring of MidiInMessage.
//
// PRODUCER: exactly one MIDI input device thread (the WinMM callback, the
// CoreMIDI read proc, the ALSA-seq poll thread, CaptureMidiInput::inject).
// CONSUMER: exactly one reader. For the live lane that reader is the
// LiveGraphPump, inside twLiveEventSource::collect (design §4); for the MIDI
// recorder (L4) it is the main thread.
//
// Two consumers on one ring would corrupt the tail silently, which is exactly
// why design D8 says the device thread writes ONE RING PER CONSUMER — a
// fan-out at the callback — rather than one shared ring everybody pops.
class MidiInRing {
public:
    // NOT named `slots`: Qt Core is on the app's include path and `slots` is
    // one of its keyword macros, so the parameter would expand to nothing and
    // the header would fail to compile the moment an app file included it -
    // the same trap twLiveTrackPlan::inserts records.
    explicit MidiInRing( std::size_t slotCount = 1024 )
        : ring_( slotCount ? slotCount : 1 ) {}

    std::size_t capacity() const { return ring_.size(); }

    // Producer side. False when the ring is FULL or the message does not fit;
    // both are counted by the caller. Never blocks, never allocates.
    bool push( const std::uint8_t *bytes, std::size_t n, std::int64_t hostNs )
    {
        if( n == 0 || n > MidiInMessage::kMaxMessageBytes ) return false;
        const std::size_t h = head_.load( std::memory_order_relaxed );
        const std::size_t next = ( h + 1 ) % ring_.size();
        if( next == tail_.load( std::memory_order_acquire ) ) return false;
        MidiInMessage &m = ring_[h];
        m.hostTimeNs = hostNs;
        m.size       = (std::uint8_t) n;
        for( std::size_t i = 0; i < n; ++i ) m.bytes[i] = bytes[i];
        head_.store( next, std::memory_order_release );
        return true;
    }

    // Consumer side. False when the ring is empty.
    bool pop( MidiInMessage &out )
    {
        const std::size_t t = tail_.load( std::memory_order_relaxed );
        if( t == head_.load( std::memory_order_acquire ) ) return false;
        out = ring_[t];
        tail_.store( ( t + 1 ) % ring_.size(), std::memory_order_release );
        return true;
    }

    bool empty() const
    {
        return tail_.load( std::memory_order_acquire )
               == head_.load( std::memory_order_acquire );
    }

    // Consumer side only, and only while the producer is known to be idle
    // (a port that has just been acquired): drop everything queued.
    void clear() { tail_.store( head_.load( std::memory_order_acquire ),
                                std::memory_order_release ); }

private:
    std::vector<MidiInMessage> ring_;
    std::atomic<std::size_t>   head_{ 0 };   // producer only
    std::atomic<std::size_t>   tail_{ 0 };   // consumer only
};

}  // namespace audio

#endif
