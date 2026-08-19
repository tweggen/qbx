#ifndef _TW_EVENTSOURCE_H_
#define _TW_EVENTSOURCE_H_

#include <cstdint>
#include <vector>

#include "tw/events/twevent.h"
#include "tw/events/tweventseq.h"

/**
 * twEventSource — the seam every event consumer reads through (proposal 37
 * §4.2): the instrument slot processor per page, and the MIDI-out pump per
 * tick. A track's event clip set is one; a track's FEED (the merge of its own
 * set with the feeds of the children that bubble events up, §3.2.1) is another;
 * neither the processor nor the pump can tell them apart.
 *
 * `collect(startPos, len, out)` answers ONE question completely: "if you know
 * nothing, what do you have to do to sound the window [startPos, startPos+len)
 * correctly?" — which is (a) the CHASE set at startPos (what is already
 * sounding, and every controller value that got it there — D4's reset + chase +
 * pre-roll) and (b) the events inside the window, at PAGE-RELATIVE times.
 *
 * That is deliberately not "the events in the window": a note that started
 * before the window would otherwise arrive as a note-on at offset 0 and
 * re-attack on every page boundary. Note-ons before the window are ONLY ever
 * reported through the chase.
 */

/**
 * The output of a collect. `events` times are relative to the collect's
 * startPos (0..len-1); `chase.notes` starts are ABSOLUTE positions in the
 * source's domain, because D4's pre-roll length is computed from how long ago
 * the held notes began.
 */
struct twEventBlock {
    twEventState         chase;
    std::vector<twEvent> events;   // sorted by time; note-offs before note-ons
    std::vector<uint8_t> arena;    // payload bytes the events index

    void clear() { chase.clear(); events.clear(); arena.clear(); }
    bool empty() const { return events.empty() && chase.empty(); }

    uint32_t addPayload(const void *data, size_t n);
    const uint8_t *payload(const twEvent &e) const {
        if (e.payloadSize == 0) return nullptr;
        if ((size_t)e.payloadOffset + e.payloadSize > arena.size()) return nullptr;
        return arena.data() + e.payloadOffset;
    }
    // Stable sort by time with note-offs ordered BEFORE note-ons at the same
    // time — a re-trigger of the same key at a loop point must release before
    // it attacks, or the instrument sees two overlapping notes.
    void sortEvents();
};

class twEventSource
{
public:
    virtual ~twEventSource() = default;

    // Clears `out` and fills it. Never blocks on a render, never allocates a
    // page, never demands anything: an event source is model data (D1).
    virtual void collect(int64_t startPos, int64_t len, twEventBlock &out) const = 0;

    // "Re-state what you are holding, at offset 0 of the next block you are
    // collected for." A no-op for a source whose material is POSITIONAL — a
    // sequenced clip re-derives everything from startPos, so there is nothing
    // to restate. It matters only for a LIVE source, whose held notes exist
    // solely in its own table: a consumer that resets its DSP has just thrown
    // those voices away and cannot rebuild them from position alone.
    //
    // const because a source is model data to its consumer and the request is
    // one atomic flag (proposal 33 follow-up / proposal 21 D4).
    virtual void requestChase() const {}
};

// ---------------------------------------------------------------------------
// Note-id namespacing (§3.2.1)
//
//   bits  0..19  the note-on's index in its sequence       (1 048 576)
//   bits 20..26  the clip slot inside one twEventClipSet   (128)
//   bits 27..30  the source index inside a twEventMerge    (16)
//   bit  31      always 0 — ids are non-negative; -1 means "no id"
//
// The composition is what makes two children playing the SAME key on the SAME
// channel two overlapping notes to the instrument instead of one truncated one.
// ---------------------------------------------------------------------------
inline constexpr int32_t TW_NOTEID_INDEX_BITS = 20;
inline constexpr int32_t TW_NOTEID_INDEX_MASK = (1 << TW_NOTEID_INDEX_BITS) - 1;
inline constexpr int32_t TW_NOTEID_SLOT_SHIFT = TW_NOTEID_INDEX_BITS;
inline constexpr int32_t TW_NOTEID_SLOT_MASK  = 0x7F;
inline constexpr int32_t TW_NOTEID_SOURCE_SHIFT = 27;
inline constexpr int32_t TW_NOTEID_SOURCE_MASK  = 0xF;

inline int32_t twMakeNoteId(int32_t clipSlot, int64_t eventIndex) {
    return ((clipSlot & TW_NOTEID_SLOT_MASK) << TW_NOTEID_SLOT_SHIFT)
         | ((int32_t)eventIndex & TW_NOTEID_INDEX_MASK);
}
inline int32_t twNamespaceNoteId(int32_t id, int32_t sourceIndex) {
    if (id < 0) return id;   // "no id" survives every namespacing
    return id | ((sourceIndex & TW_NOTEID_SOURCE_MASK) << TW_NOTEID_SOURCE_SHIFT);
}

#endif // _TW_EVENTSOURCE_H_
