#ifndef _TW_EVENTSEQ_H_
#define _TW_EVENTSEQ_H_

#include <cstdint>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "tw/events/twevent.h"

/**
 * twEventSeq — an IMMUTABLE, sorted, binary-searchable event table plus the
 * byte arena its payload offsets index (proposal 36 D1).
 *
 * Events are model data, not pages: there is no event page kind, no planPage
 * and no scheduler edge for them. A consumer slices the table by position and
 * asks `stateAt(P)` what is sounding at a position — the question a page-based
 * design would have had to bake into every page.
 *
 * Immutability is the threading story: an edit builds a NEW sequence and the
 * owner swaps the shared_ptr under its mutex, so a freeze that already holds
 * one keeps a coherent table for its whole duration (THREADING rule 2).
 *
 * The DOMAIN of `time` belongs to the owner, not to this class: an
 * SMidiSequence's table is in musical TICKS, a twEventClipSet's resolved
 * sequences are read in whatever domain their map produces. Nothing here
 * converts.
 */

// One note sounding at a queried position (the chase set's note half).
struct twHeldNote {
    int16_t channel = -1;
    int16_t key = -1;
    int32_t noteId = -1;
    double  velocity = 0.0;
    int64_t start = 0;     // where the note-on was, in the query's domain
    int64_t duration = 0;  // 0 = still open (paired representation)
    int64_t srcIndex = -1; // index of the note-on in the sequence it came from.
                           // twEventClipSet mints the note id from it, which is
                           // what makes an id STABLE across collect() calls —
                           // the note-off of a note chased in one page must
                           // carry the id its note-on carried in the previous
                           // one, and a per-call counter cannot do that.

    bool operator==(const twHeldNote &o) const {
        return channel == o.channel && key == o.key && noteId == o.noteId
            && velocity == o.velocity && start == o.start
            && duration == o.duration && srcIndex == o.srcIndex;
    }
};

/**
 * Everything a consumer must be told to continue correctly from a position it
 * did not render up to — the CHASE set of D4 (reset + chase + pre-roll).
 * Ordered containers, so two states built by different routes compare equal
 * and a chase is emitted in a deterministic order.
 */
struct twEventState {
    std::vector<twHeldNote>                 notes;    // sorted by (start, channel, key, noteId)
    std::map<std::pair<int16_t, uint32_t>, double> cc; // last value per (channel, cc)
    std::map<int16_t, double>               bend;     // last per channel
    std::map<int16_t, double>               pressure; // last channel pressure
    std::map<int16_t, int32_t>              program;  // last program per channel
    std::map<int16_t, bool>                 sustain;  // CC64 >= 64 per channel

    bool empty() const {
        return notes.empty() && cc.empty() && bend.empty() && pressure.empty()
            && program.empty() && sustain.empty();
    }
    void clear() {
        notes.clear(); cc.clear(); bend.clear(); pressure.clear();
        program.clear(); sustain.clear();
    }
    // Fold `o` on top of this one: later wins for the scalar channels, notes
    // are concatenated. The union twEventMerge's chase is.
    void mergeOver(const twEventState &o);
    void sortNotes();

    bool operator==(const twEventState &o) const {
        return notes == o.notes && cc == o.cc && bend == o.bend
            && pressure == o.pressure && program == o.program
            && sustain == o.sustain;
    }
    bool operator!=(const twEventState &o) const { return !(*this == o); }
};

class twEventSeq
{
public:
    twEventSeq() = default;
    // Takes the table AND the arena the payload offsets index. The table is
    // sorted here (stable, by time) so no caller can hand in an unsorted one.
    twEventSeq(std::vector<twEvent> events, std::vector<uint8_t> arena);

    const std::vector<twEvent> &events() const { return events_; }
    size_t size() const { return events_.size(); }
    bool   empty() const { return events_.empty(); }

    const uint8_t *payload(const twEvent &e) const {
        if (e.payloadSize == 0) return nullptr;
        if ((size_t)e.payloadOffset + e.payloadSize > arena_.size()) return nullptr;
        return arena_.data() + e.payloadOffset;
    }
    const std::vector<uint8_t> &arena() const { return arena_; }

    // Half-open index range of the events with time in [a, b). O(log n).
    std::pair<size_t, size_t> slice(int64_t a, int64_t b) const;
    // First index with time >= t.
    size_t lowerBound(int64_t t) const;

    // What is sounding / what the controllers hold at P, counting every event
    // with time < P (and, for notes stored with a duration, every note whose
    // half-open [start, start+duration) contains P).
    //
    // Both note representations are honoured: a NoteOn with duration > 0 ends
    // by itself; a NoteOn with duration <= 0 stays open until a matching
    // NoteOff (same channel+key) closes it, FIFO — that is what makes a
    // sequence built from a live capture work as well as one built from a file.
    twEventState stateAt(int64_t p) const;

    // The end of the last thing in the table (max over time, and over
    // time+duration for notes). 0 for an empty sequence.
    int64_t endPosition() const;

    // Equality including the payload BYTES — the round-trip gate's comparison.
    static bool equalTables(const twEventSeq &a, const twEventSeq &b);

private:
    std::vector<twEvent>  events_;
    std::vector<uint8_t>  arena_;
};

/**
 * The only way to build a sequence: collect events and payloads, then `build()`
 * (which sorts). Copying a payload in returns the offset to store.
 */
class twEventSeqBuilder
{
public:
    void add(const twEvent &e) { events_.push_back(e); }
    // Copies `n` bytes into the arena and stamps offset/size onto `e`.
    void addWithPayload(twEvent e, const void *data, size_t n);
    uint32_t addPayload(const void *data, size_t n);

    size_t size() const { return events_.size(); }
    void reserve(size_t n) { events_.reserve(n); }

    std::shared_ptr<const twEventSeq> build();

private:
    std::vector<twEvent> events_;
    std::vector<uint8_t> arena_;
};

#endif // _TW_EVENTSEQ_H_
