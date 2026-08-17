#include "tw/devices/twliveeventsource.h"

#include <algorithm>

namespace audio {

namespace {

// The block-relative offset a "now" event gets. Clamping to 0 (design D4)
// rather than dropping is the whole point; see the header.
constexpr std::size_t kPendingReserve = 256;
constexpr std::size_t kHeldReserve    = 64;

}  // namespace

twLiveEventSource::twLiveEventSource( MidiInFanout::Sink *sink, int sampleRate )
    : sink_( sink )
{
    rate_.store( sampleRate > 0 ? sampleRate : 48000, std::memory_order_relaxed );
    // Sized ON THE MAIN THREAD so the pump's drain allocates nothing (design
    // §4). They only ever grow past this if a performer really does hold 64
    // notes, and a one-off growth then is better than a fixed cap that eats
    // events.
    held_.reserve( kHeldReserve );
    pending_.reserve( kPendingReserve );
}

twLiveEventSource::~twLiveEventSource() = default;

void twLiveEventSource::setClock( std::shared_ptr<const twLiveFrameClock> clock )
{
    std::lock_guard<std::mutex> lk( clockMutex_ );
    clock_ = std::move( clock );
}

void twLiveEventSource::setLatencyFrames( offset_t frames )
{
    latency_.store( frames, std::memory_order_relaxed );
}

void twLiveEventSource::setSampleRate( int rate )
{
    if( rate > 0 ) rate_.store( rate, std::memory_order_relaxed );
}

void twLiveEventSource::requestChase()
{
    chaseReq_.store( true, std::memory_order_release );
}

void twLiveEventSource::requestAllNotesOff()
{
    panicReq_.store( true, std::memory_order_release );
}

std::size_t twLiveEventSource::heldNotes() const
{
    return held_.size();
}

void twLiveEventSource::fillChase( twEventBlock &out ) const
{
    for( const Held &h : held_ ) {
        twHeldNote n;
        n.channel  = (std::int16_t) h.channel;
        n.key      = (std::int16_t) h.key;
        n.noteId   = h.noteId;
        n.velocity = h.velocity;
        n.start    = h.start;
        n.duration = 0;          // still open
        n.srcIndex = -1;
        out.chase.notes.push_back( n );
    }
    out.chase.sortNotes();
    for( const auto &kv : cc_ )       out.chase.cc[kv.first]       = kv.second;
    for( const auto &kv : bend_ )     out.chase.bend[kv.first]     = kv.second;
    for( const auto &kv : pressure_ ) out.chase.pressure[kv.first] = kv.second;
    for( const auto &kv : program_ )  out.chase.program[kv.first]  = kv.second;
    for( const auto &kv : cc_ )
        if( kv.first.second == 64 ) out.chase.sustain[kv.first.first] = kv.second >= 64.0;
}

void twLiveEventSource::emitOne( const MidiInMessage &m, offset_t frame,
                                 std::int64_t offset, twEventBlock &out ) const
{
    if( m.size < 2 ) return;
    const std::uint8_t st = m.bytes[0];
    if( st < 0x80 || st >= 0xF0 ) return;    // system / realtime: not ours (yet)

    const std::int16_t ch = (std::int16_t)( st & 0x0F );
    const std::uint8_t d1 = m.bytes[1];
    const std::uint8_t d2 = ( m.size >= 3 ) ? m.bytes[2] : 0;

    twEvent e;
    e.time    = offset;
    e.channel = ch;
    e.flags   = twEventIsLive;

    switch( st & 0xF0 ) {
    case 0x90:                                   // note on (velocity 0 == off)
        if( d2 > 0 ) {
            e.kind  = twEventKind::NoteOn;
            e.key   = (std::int16_t) d1;
            e.value = (double) d2;
            // A fresh id per note-on, so two overlapping presses of the SAME
            // key on the SAME channel are two notes to the instrument. The
            // processor namespaces it again (kLiveNoteIdSource) on the way in.
            e.noteId = ( nextNoteId_++ ) & TW_NOTEID_INDEX_MASK;
            Held h;
            h.channel  = ch;
            h.key      = d1;
            h.noteId   = e.noteId;
            h.velocity = (double) d2;
            h.start    = frame;
            held_.push_back( h );
            break;
        }
        // fall through: a note-on with velocity 0 IS a note-off
        [[fallthrough]];
    case 0x80: {                                 // note off
        e.kind  = twEventKind::NoteOff;
        e.key   = (std::int16_t) d1;
        e.value = (double) ( ( ( st & 0xF0 ) == 0x80 ) ? d2 : 0 );
        // FIFO against the held table, so the id matches the note-on this
        // release belongs to. Without it the instrument gets a note-off for a
        // note it never heard of and the voice hangs.
        auto it = std::find_if( held_.begin(), held_.end(),
                                [&]( const Held &h ) {
                                    return h.channel == ch && h.key == (int) d1;
                                } );
        if( it != held_.end() ) {
            e.noteId = it->noteId;
            held_.erase( it );
        }
        break;
    }
    case 0xA0:
        e.kind  = twEventKind::PolyPressure;
        e.key   = (std::int16_t) d1;
        e.value = (double) d2;
        break;
    case 0xB0:
        e.kind    = twEventKind::ControlChange;
        e.paramId = d1;
        e.value   = (double) d2;
        cc_[{ ch, (std::uint32_t) d1 }] = (double) d2;
        break;
    case 0xC0:
        e.kind  = twEventKind::ProgramChange;
        e.value = (double) d1;
        program_[ch] = (std::int32_t) d1;
        break;
    case 0xD0:
        e.kind  = twEventKind::ChannelPressure;
        e.value = (double) d1;
        pressure_[ch] = (double) d1;
        break;
    case 0xE0: {
        // The MODEL's pitch-bend domain is -8192 .. +8191 (twNormalizeForAbi),
        // so the 14-bit wire value is centred here and nowhere else.
        const int raw = (int) d1 | ( (int) d2 << 7 );
        e.kind  = twEventKind::PitchBend;
        e.value = (double) ( raw - 8192 );
        bend_[ch] = e.value;
        break;
    }
    default:
        return;
    }

    out.events.push_back( e );
    emitted_.fetch_add( 1, std::memory_order_relaxed );
}

void twLiveEventSource::collect( std::int64_t startPos, std::int64_t len,
                                 twEventBlock &out ) const
{
    out.clear();
    if( len <= 0 ) return;

    std::shared_ptr<const twLiveFrameClock> clock;
    {
        std::lock_guard<std::mutex> lk( clockMutex_ );
        clock = clock_;
    }

    const int      rate    = rate_.load( std::memory_order_relaxed );
    const offset_t latency = latency_.load( std::memory_order_relaxed );

    // 1. THE FLUSH, before anything new. A disarm asks for it, and it has to
    //    be the first thing in the block or a note released here could be
    //    re-attacked by a chase in the same one.
    if( panicReq_.exchange( false, std::memory_order_acq_rel ) ) {
        for( const Held &h : held_ ) {
            twEvent e;
            e.time    = 0;
            e.kind    = twEventKind::NoteOff;
            e.channel = (std::int16_t) h.channel;
            e.key     = (std::int16_t) h.key;
            e.noteId  = h.noteId;
            e.value   = 0.0;
            e.flags   = twEventIsLive | twEventSynthesisedOff;
            out.events.push_back( e );
        }
        held_.clear();
        pending_.clear();
    }

    // 2. THE ONE CHASE AT LIVE START (design D4). The held table becomes
    //    note-ons at offset 0 AND the block's chase set, because the two
    //    consumers differ: the generator's normal path replays `events`, its
    //    pre-roll path replays `chase`.
    const bool chase = chaseReq_.exchange( false, std::memory_order_acq_rel );
    if( chase ) {
        for( const auto &kv : cc_ ) {
            twEvent e;
            e.time    = 0;
            e.kind    = twEventKind::ControlChange;
            e.channel = kv.first.first;
            e.paramId = kv.first.second;
            e.value   = kv.second;
            e.flags   = twEventIsLive;
            out.events.push_back( e );
        }
        for( const auto &kv : bend_ ) {
            twEvent e;
            e.time = 0; e.kind = twEventKind::PitchBend;
            e.channel = kv.first; e.value = kv.second; e.flags = twEventIsLive;
            out.events.push_back( e );
        }
        for( const Held &h : held_ ) {
            twEvent e;
            e.time    = 0;
            e.kind    = twEventKind::NoteOn;
            e.channel = (std::int16_t) h.channel;
            e.key     = (std::int16_t) h.key;
            e.noteId  = h.noteId;
            e.value   = h.velocity;
            e.flags   = twEventIsLive;
            out.events.push_back( e );
        }
    }

    // 3. WHAT WAS DEFERRED LAST TIME, then the ring. In that order, because a
    //    deferred event is by definition older than anything still in the ring.
    const std::int64_t end = startPos + len;
    std::vector<Deferred> stillPending;
    stillPending.reserve( pending_.size() );

    auto place = [&]( const MidiInMessage &m, offset_t frame ) {
        std::int64_t off = (std::int64_t) frame - startPos;
        if( off < 0 ) {
            off = 0;
            clamped_.fetch_add( 1, std::memory_order_relaxed );
        }
        if( off >= len ) {
            Deferred d;
            d.frame = frame;
            d.msg   = m;
            stillPending.push_back( d );
            deferred_.fetch_add( 1, std::memory_order_relaxed );
            return;
        }
        emitOne( m, (offset_t)( startPos + off ), off, out );
    };

    for( const Deferred &d : pending_ )
        place( d.msg, d.frame );
    pending_.clear();

    if( sink_ ) {
        MidiInMessage m;
        while( sink_->pop( m ) ) {
            // The key was pressed BEFORE the byte arrived: the port's own
            // latency plus the user's correction, both already folded into
            // `latency` in frames by the app.
            offset_t frame = startPos;
            if( clock )
                frame = clock->frameForHostTime( m.hostTimeNs, (offset_t) startPos,
                                                 (length_t) len );
            frame -= latency;
            place( m, frame );
        }
    }
    pending_.swap( stillPending );

    // 4. The block's chase set is the state as it now stands - what a consumer
    //    would have to be told to continue correctly. It is filled whether or
    //    not a chase was requested: a pre-roll may ask for it at any time, and
    //    it costs nothing when nothing is held.
    fillChase( out );
    out.sortEvents();

    (void) rate;
    (void) end;
}

}  // namespace audio
