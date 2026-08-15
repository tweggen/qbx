#ifndef _TWPLUGINEVENTS_H_
#define _TWPLUGINEVENTS_H_

#include "tw/events/twevent.h"

#include <cstdint>
#include <cstring>
#include <string>

/**
 * tw/plugins — the EVENT half of the plugin ABI (proposal 36 §5.1, P2).
 *
 * This header is deliberately format-free (plugins/CONTRACT.md invariant 4).
 * There is no clap_*, no Steinberg::, no AudioUnit type anywhere in it, and
 * there must never be one: every backend header stays PRIVATE to tw_plugins,
 * so a declaration that changed shape with TW_HAVE_CLAP / TW_HAVE_VST3 /
 * TW_HAVE_AU would give tw_plugins and its consumers different views of the
 * same type (ODR/ABI skew).
 *
 * It also does not define an event. `twEvent` and `twEventKind` come from
 * tw/events/twevent.h and are THE ones (proposal 36 §4.1, review #2): the
 * sequence snapshots, the event clip set, this ABI and the MIDI-out pump all
 * quote the same struct. That is why tw_plugins links tw_events.
 *
 * THE TIME DOMAIN. Inside a twEventList handed to process(), `twEvent::time`
 * is CHUNK-RELATIVE: 0 .. nframes-1 of THAT call, never a project position.
 * The position of frame 0 is in twProcessContext::position instead. The same
 * field means a position in a twEventSeq; the two uses are documented in
 * twevent.h and there is no third.
 *
 * WHAT NEVER ARRIVES. Metadata kinds (Tempo, TimeSig, Marker, Lyric, ...) are
 * sequence-only — twEventIsMetadata() is true for them and a host must filter
 * them out before the call. A backend may assume it never sees one.
 *
 * ALLOCATION. Nothing here allocates. The host owns the event storage and the
 * payload arena; a backend sizes its own translation buffers in prepare() and
 * process() only fills them (plugins/CONTRACT.md invariant 2).
 */

namespace audio {

// Sizing constants. A host sizes its list from these, and a backend reserves
// its translation buffers for them in prepare(); neither may exceed them.
namespace twEventLimits {

// The most events one process() call may carry. 4096-frame chunks
// (twPluginSlotProcessor::kChunkFrames) at a musically absurd 8 events per
// millisecond is ~680; 1024 leaves headroom and keeps the reserve small.
constexpr std::uint32_t kMaxEventsPerBlock = 1024;

// The payload arena a host must be able to hand over per call (sysex, text).
constexpr std::uint32_t kMaxPayloadBytes = 65536;

}  // namespace twEventLimits

// ---------------------------------------------------------------------------
// twEventList — the host's events for ONE process() call.
//
// A plain view, not a container: `events` and `payloads` point into storage the
// HOST owns and that is valid only for the duration of the call. A backend that
// needs an event to outlive the call must copy it (nothing does today).
//
// INVARIANT: sorted by `time`, non-decreasing. Every backend forwards the order
// it is given, and CLAP and VST3 both require sorted input, so an unsorted list
// is a host bug that shows up as a plugin refusing to process.
// ---------------------------------------------------------------------------
struct twEventList {
    const twEvent *events   = nullptr;
    std::uint32_t  count    = 0;
    // The arena `payloadOffset`/`payloadSize` index. May be null when no event
    // in the list carries a payload.
    const std::uint8_t *payloads     = nullptr;
    std::uint32_t       payloadBytes = 0;

    bool           empty() const { return count == 0; }
    const twEvent &at( std::uint32_t i ) const { return events[i]; }

    // The bytes of a payload-carrying event, or nullptr when the event carries
    // none or its span does not fit the arena (a malformed list must not make a
    // backend read out of bounds).
    const std::uint8_t *payloadOf( const twEvent &e ) const
    {
        if( e.payloadSize == 0 || !payloads )
            return nullptr;
        if( (std::uint64_t)e.payloadOffset + e.payloadSize > (std::uint64_t)payloadBytes )
            return nullptr;
        return payloads + e.payloadOffset;
    }
};

// ---------------------------------------------------------------------------
// twEventOut — the plugin -> host sink for ONE process() call.
//
// The host provides the storage (again: no allocation on the render path) and
// reads back what landed. OVERFLOW IS COUNTED AND DROPPED, never grown and
// never an error: a plugin that pushes more than the host sized for loses the
// surplus and the host can report `dropped()`. Silently succeeding would hide a
// mis-sized host; failing the render would turn a chatty arpeggiator into a
// dead track.
// ---------------------------------------------------------------------------
class twEventOut {
public:
    twEventOut() = default;

    // Sized by the host in ITS prepare(); storage must outlive the call.
    void setStorage( twEvent *events, std::uint32_t capacity,
                     std::uint8_t *payloadArena = nullptr,
                     std::uint32_t payloadCapacity = 0 )
    {
        events_          = events;
        capacity_        = events ? capacity : 0;
        payloads_        = payloadArena;
        payloadCapacity_ = payloadArena ? payloadCapacity : 0;
        clear();
    }

    void clear()
    {
        count_       = 0;
        payloadUsed_ = 0;
        dropped_     = 0;
    }

    // Returns false when the event did not fit (and counts the drop).
    bool push( const twEvent &e )
    {
        if( count_ >= capacity_ ) {
            ++dropped_;
            return false;
        }
        events_[count_]               = e;
        events_[count_].payloadOffset = 0;
        events_[count_].payloadSize   = 0;
        ++count_;
        return true;
    }

    // Copies `n` payload bytes into the sink's own arena and rebases the
    // event's offset onto it — the caller's bytes need not outlive the call.
    bool pushWithPayload( const twEvent &e, const std::uint8_t *bytes, std::uint32_t n )
    {
        if( n == 0 || !bytes )
            return push( e );
        if( count_ >= capacity_ || payloadUsed_ + n > payloadCapacity_ ) {
            ++dropped_;
            return false;
        }
        std::memcpy( payloads_ + payloadUsed_, bytes, n );
        events_[count_]               = e;
        events_[count_].payloadOffset = payloadUsed_;
        events_[count_].payloadSize   = n;
        payloadUsed_ += n;
        ++count_;
        return true;
    }

    std::uint32_t  count() const { return count_; }
    std::uint32_t  dropped() const { return dropped_; }
    std::uint32_t  capacity() const { return capacity_; }
    const twEvent &at( std::uint32_t i ) const { return events_[i]; }
    const twEvent *events() const { return events_; }
    const std::uint8_t *payloads() const { return payloads_; }
    std::uint32_t       payloadBytes() const { return payloadUsed_; }

private:
    twEvent      *events_          = nullptr;
    std::uint32_t capacity_        = 0;
    std::uint32_t count_           = 0;
    std::uint8_t *payloads_        = nullptr;
    std::uint32_t payloadCapacity_ = 0;
    std::uint32_t payloadUsed_     = 0;
    std::uint32_t dropped_         = 0;
};

// ---------------------------------------------------------------------------
// twProcessContext — where in the project this call is, and what the transport
// is doing (proposal 36 F5: today NOTHING reaches a plugin, so an arpeggiator
// cannot sync and a plugin's own position is free-running).
//
// `validFlags` is not decoration: a host that does not know the tempo must say
// so rather than send 120, because a plugin cannot tell a real 120 from a
// default one. Every consumer checks the bit before reading the field.
// ---------------------------------------------------------------------------
enum twProcessContextValid : std::uint32_t {
    twCtxNone        = 0,
    twCtxPosition    = 1u << 0,   // `position` is a real project frame position
    twCtxTempo       = 1u << 1,   // `tempoBpm`
    twCtxTimeSig     = 1u << 2,   // `tsNum` / `tsDen`
    twCtxPpqPosition = 1u << 3    // `ppqPos`
};

struct twProcessContext {
    // Project frame position of frame 0 of this call. Meaningful only with
    // twCtxPosition set; the legacy pull path is positionless and sets nothing.
    std::int64_t  position = 0;
    bool          playing  = false;
    double        tempoBpm = 120.0;
    double        ppqPos   = 0.0;   // quarter notes since the project start
    std::int32_t  tsNum    = 4;
    std::int32_t  tsDen    = 4;
    std::uint32_t validFlags = twCtxNone;

    bool has( twProcessContextValid f ) const { return ( validFlags & f ) != 0; }
};

// ---------------------------------------------------------------------------
// twPluginCapabilities — what the instance can do with events.
//
// Queried, never assumed. `acceptsNotes()` on twPlugin stays as a forwarder to
// `capabilities().acceptsNotes` for one release so no caller breaks.
// ---------------------------------------------------------------------------
struct twPluginCapabilities {
    bool acceptsNotes = false;   // has at least one note/event INPUT port
    bool emitsNotes   = false;   // has at least one note/event OUTPUT port
    bool isInstrument = false;   // declares itself a generator

    // The plugin prefers raw MIDI 1.0 bytes over structured note events. CLAP
    // says so per note port (dialect negotiation); AU has no other dialect.
    bool wantsMidi1Raw = false;

    // A NoteOff can be matched to its NoteOn by `noteId` rather than by
    // (port, channel, key). False means the backend must not rely on ids.
    bool supportsNoteIds = false;

    // Per-note expression (CLAP note expressions / VST3 kNoteExpressionValue).
    bool supportsNoteExpression = false;

    // The plugin may push parameter changes back through twEventOut.
    bool emitsParamChanges = false;

    std::uint16_t notePortsIn  = 0;
    std::uint16_t notePortsOut = 0;
};

// ---------------------------------------------------------------------------
// twPluginBusInfo — one AUDIO OUTPUT bus.
//
// The main bus is what twPluginIoLayout::audioOutputs already describes; the
// rest are aux outputs, which every backend has been reading and DISCARDING
// since proposal 08. Reporting them is what proposal 36 §5.4 needs to route an
// aux tap to a return track later; nothing consumes bus > 0 yet.
// ---------------------------------------------------------------------------
struct twPluginBusInfo {
    std::uint16_t channels = 0;
    bool          isMain   = false;
    std::string   name;
};

}  // namespace audio

#endif  // _TWPLUGINEVENTS_H_
