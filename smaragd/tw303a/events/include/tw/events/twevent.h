#ifndef _TW_EVENT_H_
#define _TW_EVENT_H_

#include <cstdint>

/**
 * tw/events — THE one event type (proposal 36 §4.1).
 *
 * There is exactly one twEvent in this codebase. The engine's sequence
 * snapshots, the event clip set, the plugin ABI's event list and the MIDI-out
 * pump all use THIS struct; a second, format-flavoured event type is what the
 * proposal's review #2 rejected. The struct is pinned — fields are added at the
 * end, never renamed or reordered, because both the ABI and the file format
 * quote it.
 *
 * `time` has two documented uses and no third:
 *   - in a twEventSeq / twEventClipSet: a POSITION in the owner's domain —
 *     frames (ClipPos) for a clip set, musical TICKS for the content a
 *     twSmf import produces (the owner declares which; see CONTRACT.md);
 *   - in a plugin process() call: CHUNK-RELATIVE, 0..n-1.
 *
 * `duration` exists only in a SEQUENCE: notes are stored WITH their length
 * (the SMF reader pairs note-on/note-off into one event) so a slice can answer
 * "what is held here" without replaying the whole stream. A NoteOn handed to a
 * plugin carries duration 0 and is always followed by its own NoteOff.
 *
 * Payloads (sysex bytes, text, an unknown meta blob) are NOT in the struct:
 * `payloadOffset`/`payloadSize` index the OWNER's byte arena. A twEventSeq owns
 * its arena; the ABI's list points into a host arena valid for the call.
 */

enum class twEventKind : uint8_t {
    NoteOn, NoteOff, NoteChoke, NoteEnd, NoteExpression, PolyPressure,
    ControlChange, PitchBend, ChannelPressure, ProgramChange, Sysex, Midi1,
    ParamValue, ParamMod, ParamGestureBegin, ParamGestureEnd, Transport,
    // metadata (SMF meta + notation/tab/tracker) — sequence-only, never sent
    // to a plugin
    Tempo, TimeSig, KeySig, Marker, Lyric, ChordSymbol, Articulation,
    StringFret, TrackerCell, Text, NoteAttr,
    Unknown = 0xF0
};

// `flags` bits.
enum twEventFlags : uint8_t {
    twEventFlagNone        = 0,
    twEventIsLive          = 1u << 0,  // arrived from a device this run
    twEventDontRecord      = 1u << 1,  // audible, but not captured
    twEventMuted           = 1u << 2,  // present in the table, not sounded
    twEventSynthesisedOff  = 1u << 3   // a NoteOff this engine invented (clip
                                       // end / loop wrap), not one the content
                                       // holds — see twEventClipSet
};

struct twEvent {
    int64_t     time = 0;      // see the header comment: position OR chunk-relative
    twEventKind kind = twEventKind::Unknown;
    uint8_t     flags = twEventFlagNone;
    int16_t     port = -1;     // -1 = wildcard / n/a
    int16_t     channel = -1;
    int16_t     key = -1;
    int32_t     noteId = -1;   // host-issued; -1 = none
    uint32_t    paramId = 0;   // ParamValue/Mod/Gesture/NoteExpression type;
                               // for a metadata kind: the SMF meta byte
    double      value = 0.0;   // velocity / CC value / bend / param / expression
    double      value2 = 0.0;  // tuning cents, bank, time-signature denominator, …
    int64_t     duration = 0;  // NoteOn in a SEQUENCE only
    uint32_t    payloadOffset = 0;  // → the OWNER's arena
    uint32_t    payloadSize = 0;
};

// True for the kinds that describe the SCORE rather than a performance gesture.
// They live in sequences and files; they are never handed to a plugin.
inline bool twEventIsMetadata(twEventKind k) {
    switch (k) {
    case twEventKind::Tempo:      case twEventKind::TimeSig:
    case twEventKind::KeySig:     case twEventKind::Marker:
    case twEventKind::Lyric:      case twEventKind::ChordSymbol:
    case twEventKind::Articulation: case twEventKind::StringFret:
    case twEventKind::TrackerCell: case twEventKind::Text:
    case twEventKind::NoteAttr:   case twEventKind::Unknown:
        return true;
    default:
        return false;
    }
}

// Field-by-field equality — the identity the round-trip gates compare with.
// Payload BYTES are not compared here (the offsets index different arenas);
// twEventSeq::equalTables does that.
inline bool twEventSameFields(const twEvent &a, const twEvent &b) {
    return a.time == b.time && a.kind == b.kind && a.flags == b.flags
        && a.port == b.port && a.channel == b.channel && a.key == b.key
        && a.noteId == b.noteId && a.paramId == b.paramId
        && a.value == b.value && a.value2 == b.value2
        && a.duration == b.duration && a.payloadSize == b.payloadSize;
}

#endif // _TW_EVENT_H_
