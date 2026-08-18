#include "tw/devices/midi_in_fanout.h"

#include "tw/core/twsyslog.h"
#include "tw/devices/midi_out_scheduler.h"

namespace audio {

MidiInFanout::Sink *MidiInFanout::acquire( std::uint16_t channelMask,
                                           const char *label )
{
    for( int i = 0; i < kMaxSinks; ++i ) {
        Sink &s = sinks_[i];
        if( s.active.load( std::memory_order_acquire ) ) continue;
        // Whatever the device thread left in this sink belongs to a PREVIOUS
        // consumer. Clearing BEFORE the flag goes up is what makes an acquire
        // hand out an empty ring: the producer does not push into an inactive
        // sink, so nothing can arrive between the clear and the store.
        s.ring.clear();
        s.channelMask.store( channelMask ? channelMask : (std::uint16_t) 0xFFFF,
                             std::memory_order_relaxed );
        s.pushed.store( 0, std::memory_order_relaxed );
        s.dropped.store( 0, std::memory_order_relaxed );
        s.active.store( true, std::memory_order_release );
        return &s;
    }
    syslog( LOG_WARNING,
            "midi-in: no free fan-out sink for '%s' (%d in use); this consumer "
            "will not receive input",
            label ? label : "?", kMaxSinks );
    return nullptr;
}

void MidiInFanout::release( Sink *s )
{
    if( !s ) return;
    s->active.store( false, std::memory_order_release );
}

bool MidiInFanout::setThru( MidiOutScheduler *sched, int channelOverride )
{
    MidiOutScheduler *have = thru_.load( std::memory_order_acquire );
    if( have && sched && have != sched ) {
        syslog( LOG_WARNING,
                "midi-in: a MIDI-thru target is already set for this port; the "
                "immediate ring is single-producer, so the second request is "
                "refused rather than raced" );
        return false;
    }
    thruChannel_.store( channelOverride, std::memory_order_relaxed );
    thru_.store( sched, std::memory_order_release );
    return true;
}

void MidiInFanout::clearThru()
{
    thru_.store( nullptr, std::memory_order_release );
    thruChannel_.store( -1, std::memory_order_relaxed );
}

void MidiInFanout::onMessage( const std::uint8_t *bytes, std::size_t n,
                              std::int64_t hostTimeNs )
{
    if( !bytes || n == 0 ) return;
    messages_.fetch_add( 1, std::memory_order_relaxed );

    const bool voice = ( n >= 2 && bytes[0] >= 0x80 && bytes[0] < 0xF0 );
    const int  ch    = voice ? ( bytes[0] & 0x0F ) : -1;

    for( int i = 0; i < kMaxSinks; ++i ) {
        Sink &s = sinks_[i];
        if( !s.active.load( std::memory_order_acquire ) ) continue;
        if( voice ) {
            const std::uint16_t mask =
                s.channelMask.load( std::memory_order_relaxed );
            if( !( mask & (std::uint16_t)( 1u << ch ) ) ) continue;
        }
        if( s.ring.push( bytes, n, hostTimeNs ) )
            s.pushed.fetch_add( 1, std::memory_order_relaxed );
        else
            s.dropped.fetch_add( 1, std::memory_order_relaxed );
    }

    // THRU, on the device thread, straight into the scheduler's IMMEDIATE ring
    // (design D8). Never enqueue(): that ring's single producer is the app's
    // main-thread MIDI-out pump.
    if( MidiOutScheduler *sched = thru_.load( std::memory_order_acquire ) ) {
        if( n > MidiInMessage::kMaxMessageBytes ) {
            thruDropped_.fetch_add( 1, std::memory_order_relaxed );
            return;
        }
        std::uint8_t out[MidiInMessage::kMaxMessageBytes];
        for( std::size_t i = 0; i < n; ++i ) out[i] = bytes[i];
        const int over = thruChannel_.load( std::memory_order_relaxed );
        if( voice && over >= 0 && over < 16 )
            out[0] = (std::uint8_t)( ( out[0] & 0xF0 ) | (std::uint8_t) over );
        if( sched->sendImmediate( out, n ) )
            thruSent_.fetch_add( 1, std::memory_order_relaxed );
        else
            thruDropped_.fetch_add( 1, std::memory_order_relaxed );
    }
}

MidiInputCallback MidiInFanout::callback()
{
    return [this]( const std::uint8_t *b, std::size_t n, std::int64_t t ) {
        this->onMessage( b, n, t );
    };
}

}  // namespace audio
