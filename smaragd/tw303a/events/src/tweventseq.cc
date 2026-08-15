#include "tw/events/tweventseq.h"

#include <algorithm>
#include <cstring>

// ---------------------------------------------------------------------------
// twEventState
// ---------------------------------------------------------------------------

void twEventState::sortNotes()
{
    std::stable_sort(notes.begin(), notes.end(),
        [](const twHeldNote &a, const twHeldNote &b) {
            if (a.start != b.start)     return a.start < b.start;
            if (a.channel != b.channel) return a.channel < b.channel;
            if (a.key != b.key)         return a.key < b.key;
            return a.noteId < b.noteId;
        });
}

void twEventState::mergeOver(const twEventState &o)
{
    notes.insert(notes.end(), o.notes.begin(), o.notes.end());
    for (const auto &kv : o.cc)       cc[kv.first] = kv.second;
    for (const auto &kv : o.bend)     bend[kv.first] = kv.second;
    for (const auto &kv : o.pressure) pressure[kv.first] = kv.second;
    for (const auto &kv : o.program)  program[kv.first] = kv.second;
    for (const auto &kv : o.sustain)  sustain[kv.first] = kv.second;
    sortNotes();
}

// ---------------------------------------------------------------------------
// twEventSeq
// ---------------------------------------------------------------------------

twEventSeq::twEventSeq(std::vector<twEvent> events, std::vector<uint8_t> arena)
    : events_(std::move(events)), arena_(std::move(arena))
{
    // STABLE: events at the same position keep the order their author gave
    // them, which is what makes a file round-trip byte-identically.
    std::stable_sort(events_.begin(), events_.end(),
        [](const twEvent &a, const twEvent &b) { return a.time < b.time; });
}

size_t twEventSeq::lowerBound(int64_t t) const
{
    return (size_t)(std::lower_bound(events_.begin(), events_.end(), t,
        [](const twEvent &e, int64_t v) { return e.time < v; }) - events_.begin());
}

std::pair<size_t, size_t> twEventSeq::slice(int64_t a, int64_t b) const
{
    if (b <= a) { size_t i = lowerBound(a); return { i, i }; }
    return { lowerBound(a), lowerBound(b) };
}

int64_t twEventSeq::endPosition() const
{
    int64_t end = 0;
    for (const twEvent &e : events_) {
        int64_t t = e.time;
        if (e.kind == twEventKind::NoteOn && e.duration > 0) t += e.duration;
        if (t > end) end = t;
    }
    return end;
}

twEventState twEventSeq::stateAt(int64_t p) const
{
    twEventState st;
    // Notes stored open (duration <= 0) are closed by the next matching
    // NoteOff, oldest first — FIFO, the same rule the SMF reader pairs with.
    std::vector<size_t> open;   // indices into st.notes, in note-on order

    const size_t n = lowerBound(p);
    for (size_t i = 0; i < n; ++i) {
        const twEvent &e = events_[i];
        switch (e.kind) {
        case twEventKind::NoteOn: {
            if (e.duration > 0) {
                if (e.time + e.duration <= p) break;   // already finished
                twHeldNote h;
                h.channel = e.channel; h.key = e.key; h.noteId = e.noteId;
                h.velocity = e.value; h.start = e.time; h.duration = e.duration;
                h.srcIndex = (int64_t)i;
                st.notes.push_back(h);
            } else {
                twHeldNote h;
                h.channel = e.channel; h.key = e.key; h.noteId = e.noteId;
                h.velocity = e.value; h.start = e.time; h.duration = 0;
                h.srcIndex = (int64_t)i;
                st.notes.push_back(h);
                open.push_back(st.notes.size() - 1);
            }
            break;
        }
        case twEventKind::NoteOff:
        case twEventKind::NoteChoke:
        case twEventKind::NoteEnd: {
            for (size_t oi = 0; oi < open.size(); ++oi) {
                const twHeldNote &h = st.notes[open[oi]];
                if (h.channel == e.channel && h.key == e.key) {
                    st.notes.erase(st.notes.begin() + (long)open[oi]);
                    size_t gone = open[oi];
                    open.erase(open.begin() + (long)oi);
                    for (size_t &idx : open) if (idx > gone) --idx;
                    break;
                }
            }
            break;
        }
        case twEventKind::ControlChange:
            st.cc[{ e.channel, e.paramId }] = e.value;
            if (e.paramId == 64) st.sustain[e.channel] = (e.value >= 64.0);
            break;
        case twEventKind::PitchBend:
            st.bend[e.channel] = e.value;
            break;
        case twEventKind::ChannelPressure:
            st.pressure[e.channel] = e.value;
            break;
        case twEventKind::ProgramChange:
            st.program[e.channel] = (int32_t)e.value;
            break;
        default:
            break;   // metadata and everything else carries no chaseable state
        }
    }
    st.sortNotes();
    return st;
}

bool twEventSeq::equalTables(const twEventSeq &a, const twEventSeq &b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const twEvent &x = a.events_[i];
        const twEvent &y = b.events_[i];
        if (!twEventSameFields(x, y)) return false;
        if (x.payloadSize) {
            const uint8_t *px = a.payload(x);
            const uint8_t *py = b.payload(y);
            if (!px || !py) return false;
            if (std::memcmp(px, py, x.payloadSize) != 0) return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// twEventSeqBuilder
// ---------------------------------------------------------------------------

uint32_t twEventSeqBuilder::addPayload(const void *data, size_t n)
{
    const uint32_t off = (uint32_t)arena_.size();
    if (n) {
        const uint8_t *p = (const uint8_t *)data;
        arena_.insert(arena_.end(), p, p + n);
    }
    return off;
}

void twEventSeqBuilder::addWithPayload(twEvent e, const void *data, size_t n)
{
    e.payloadOffset = addPayload(data, n);
    e.payloadSize   = (uint32_t)n;
    events_.push_back(e);
}

std::shared_ptr<const twEventSeq> twEventSeqBuilder::build()
{
    auto seq = std::make_shared<const twEventSeq>(std::move(events_),
                                                  std::move(arena_));
    events_.clear();
    arena_.clear();
    return seq;
}
