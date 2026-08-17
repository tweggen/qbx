#include "tw/events/tweventclipset.h"

#include <algorithm>

// ---------------------------------------------------------------------------
// twEventBlock
// ---------------------------------------------------------------------------

uint32_t twEventBlock::addPayload(const void *data, size_t n)
{
    const uint32_t off = (uint32_t)arena.size();
    if (n) {
        const uint8_t *p = (const uint8_t *)data;
        arena.insert(arena.end(), p, p + n);
    }
    return off;
}

static int eventOrderRank(twEventKind k)
{
    // A release must precede an attack at the same instant, or a re-triggered
    // key at a loop point reaches the instrument as two overlapping notes.
    switch (k) {
    case twEventKind::NoteOff:
    case twEventKind::NoteChoke:
    case twEventKind::NoteEnd:   return 0;
    case twEventKind::NoteOn:    return 2;
    default:                     return 1;   // controllers settle before the attack
    }
}

void twEventBlock::sortEvents()
{
    std::stable_sort(events.begin(), events.end(),
        [](const twEvent &a, const twEvent &b) {
            if (a.time != b.time) return a.time < b.time;
            return eventOrderRank(a.kind) < eventOrderRank(b.kind);
        });
}

// ---------------------------------------------------------------------------
// The two standard maps
// ---------------------------------------------------------------------------

twEventMapPosFn twEventSlipMap(int64_t startOffset)
{
    return [startOffset](int64_t clipPos) {
        twEventMapping m;
        m.seqPos = clipPos + startOffset;
        m.runFrames = INT64_MAX;
        return m;
    };
}

twEventMapPosFn twEventLoopMap(int64_t startOffset, int64_t loopLength)
{
    if (loopLength <= 0) return twEventSlipMap(startOffset);
    return [startOffset, loopLength](int64_t clipPos) {
        // Floored modulus: a clip may be asked about a negative position (a
        // clip anchored ahead of its data, proposal 23), and C++'s % would
        // hand back the wrong iteration for it.
        int64_t r = clipPos % loopLength;
        if (r < 0) r += loopLength;
        twEventMapping m;
        m.seqPos = startOffset + r;
        m.runFrames = loopLength - r;
        return m;
    };
}

static twEventMapping applyMap(const twEventMapPosFn &map, int64_t clipPos)
{
    twEventMapping m;
    if (!map) { m.seqPos = clipPos; m.runFrames = INT64_MAX; return m; }
    m = map(clipPos);
    if (m.runFrames <= 0) m.runFrames = 1;   // a map that lies must not hang us
    return m;
}

// The mapping "just before" clipPos, expressed as the position its left limit
// converges to. Within a segment the map has slope 1, so this is exact - and it
// is what makes a note-off land on the boundary FRAME rather than inside the
// next loop iteration.
static int64_t mapLeftLimit(const twEventMapPosFn &map, int64_t clipPos)
{
    return applyMap(map, clipPos - 1).seqPos + 1;
}

// ---------------------------------------------------------------------------
// twEventClipSet - the list
// ---------------------------------------------------------------------------

const twEventClipSet::Entry *twEventClipSet::find_(const void *key) const
{
    for (const Entry &e : clips_) if (e.key == key) return &e;
    return nullptr;
}
twEventClipSet::Entry *twEventClipSet::find_(const void *key)
{
    for (Entry &e : clips_) if (e.key == key) return &e;
    return nullptr;
}

twFrameRange twEventClipSet::extentOf_(const Entry &e)
{
    twFrameRange r;
    const int64_t end = (e.duration > 0) ? e.startTime + e.duration : INT64_MAX;
    r.unite(e.startTime, end);
    return r;
}

twFrameRange twEventClipSet::insertClip(const void *key, int64_t startTime,
                                        int64_t duration, ResolveFn resolve)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (Entry *ex = find_(key)) {
        // Re-inserting a key REPLACES it (the resolver may be new); the
        // affected extent is then the union of both windows, like updateClip.
        twFrameRange r = extentOf_(*ex);
        ex->startTime = startTime;
        ex->duration = duration;
        ex->resolve = std::move(resolve);
        r.unite(extentOf_(*ex));
        return r;
    }
    Entry e;
    e.key = key;
    e.startTime = startTime;
    e.duration = duration;
    e.resolve = std::move(resolve);
    e.slot = nextSlot_++;
    if (nextSlot_ > TW_NOTEID_SLOT_MASK) nextSlot_ = 0;   // wraps; see CONTRACT
    clips_.push_back(std::move(e));
    return extentOf_(clips_.back());
}

twFrameRange twEventClipSet::updateClip(const void *key, int64_t startTime,
                                        int64_t duration)
{
    std::lock_guard<std::mutex> lock(mutex_);
    Entry *e = find_(key);
    if (!e) return twFrameRange();
    twFrameRange r = extentOf_(*e);           // where it WAS
    e->startTime = startTime;
    e->duration = duration;
    r.unite(extentOf_(*e));                   // and where it now IS
    return r;
}

twFrameRange twEventClipSet::removeClip(const void *key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < clips_.size(); ++i) {
        if (clips_[i].key != key) continue;
        twFrameRange r = extentOf_(clips_[i]);
        clips_.erase(clips_.begin() + (long)i);
        return r;
    }
    return twFrameRange();
}

twFrameRange twEventClipSet::setClipMuted(const void *key, bool muted)
{
    std::lock_guard<std::mutex> lock(mutex_);
    Entry *e = find_(key);
    if (!e || e->muted == muted) return twFrameRange();
    e->muted = muted;
    return extentOf_(*e);
}

twFrameRange twEventClipSet::touchClip(const void *key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const Entry *e = find_(key);
    return e ? extentOf_(*e) : twFrameRange();
}

bool twEventClipSet::hasClip(const void *key) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return find_(key) != nullptr;
}

size_t twEventClipSet::clipCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return clips_.size();
}

void twEventClipSet::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    clips_.clear();
}

// ---------------------------------------------------------------------------
// collect
// ---------------------------------------------------------------------------

namespace {

// One note this clip is sounding while collectClip_ walks the window.
struct OpenNote {
    int16_t channel = -1;
    int16_t key = -1;
    int32_t id = -1;
    int64_t endTrack = INT64_MAX;      // where the content says it stops
    int64_t boundaryTrack = INT64_MAX; // clip end / loop wrap, whichever is first
};

}  // namespace

void twEventClipSet::collectClip_(const Entry &e, int64_t startPos, int64_t len,
                                  twEventBlock &out) const
{
    const int64_t winEnd  = startPos + len;
    const int64_t clipEnd = (e.duration > 0) ? e.startTime + e.duration : INT64_MAX;

    if (e.muted) return;
    if (e.startTime >= winEnd) return;    // starts after this window
    if (startPos > clipEnd) return;       // ended before it. Note the '>': a
                                          // window starting exactly ON the clip
                                          // end still owes the terminal offs.

    const int64_t resolveAt = (startPos > e.startTime) ? startPos - e.startTime : 0;
    const twEventClipResolved res = e.resolve ? e.resolve(resolveAt)
                                              : twEventClipResolved();
    if (!res.seq) return;
    const twEventSeq &seq = *res.seq;
    const int64_t clipDur = (e.duration > 0) ? e.duration : INT64_MAX;

    std::vector<OpenNote> open;

    auto pushOpen = [&](int16_t ch, int16_t k, int32_t id, int64_t endTrack,
                        int64_t segEndTrack) {
        OpenNote o;
        o.channel = ch; o.key = k; o.id = id;
        o.endTrack = endTrack;
        o.boundaryTrack = std::min(clipEnd, segEndTrack);
        open.push_back(o);
    };
    // FIFO, like the SMF reader's pairing: the OLDEST sounding note of that
    // (channel, key) is the one a note-off releases.
    auto popOpen = [&](int16_t ch, int16_t k) -> int32_t {
        for (size_t i = 0; i < open.size(); ++i) {
            if (open[i].channel == ch && open[i].key == k) {
                int32_t id = open[i].id;
                open.erase(open.begin() + (long)i);
                return id;
            }
        }
        return -1;
    };

    // ---- (a) the chase: what is already sounding at startPos ---------------
    if (startPos >= e.startTime && startPos < clipEnd) {
        const int64_t p0 = startPos - e.startTime;
        const twEventMapping m = applyMap(res.map, p0);
        const int64_t segEndTrack = (m.runFrames == INT64_MAX)
                                  ? INT64_MAX : startPos + m.runFrames;
        const twEventState st = seq.stateAt(m.seqPos);
        twEventState chase;
        for (const twHeldNote &h : st.notes) {
            twHeldNote c = h;
            c.noteId = twMakeNoteId(e.slot, h.srcIndex);
            // Back into track frames. A note that began before the clip is
            // reported as having begun AT the clip start - that is when the
            // listener heard it, and D4 sizes the pre-roll from it.
            int64_t startTrack = startPos - (m.seqPos - h.start);
            if (startTrack < e.startTime) startTrack = e.startTime;
            c.start = startTrack;
            chase.notes.push_back(c);
            const int64_t endTrack = (h.duration > 0)
                ? startPos + (h.start + h.duration - m.seqPos) : INT64_MAX;
            pushOpen(h.channel, h.key, c.noteId, endTrack, segEndTrack);
        }
        chase.cc = st.cc; chase.bend = st.bend; chase.pressure = st.pressure;
        chase.program = st.program; chase.sustain = st.sustain;
        out.chase.mergeOver(chase);
    }

    // ---- (b) a boundary the window STARTS on still owes its note-offs ------
    // The window is half-open, so a clip end or a loop wrap that falls exactly
    // on startPos belongs to THIS window at offset 0 - and without this the
    // note hangs forever: the previous window ended before the boundary, and
    // this one starts a new segment in which the note is not open.
    {
        const int64_t p0 = startPos - e.startTime;
        bool boundary = false;
        if (p0 > 0 && p0 == clipDur) boundary = true;
        else if (p0 > 0 && p0 < clipDur) {
            const int64_t leftSeq = mapLeftLimit(res.map, p0);
            boundary = (applyMap(res.map, p0).seqPos != leftSeq);
        }
        if (boundary) {
            const twEventState st = seq.stateAt(mapLeftLimit(res.map, p0));
            for (const twHeldNote &h : st.notes) {
                twEvent off;
                off.time = 0;
                off.kind = twEventKind::NoteOff;
                off.flags = twEventSynthesisedOff;
                off.channel = h.channel; off.key = h.key;
                off.noteId = twMakeNoteId(e.slot, h.srcIndex);
                off.value = 0.0;
                out.events.push_back(off);
                popOpen(h.channel, h.key);   // released here, no longer open
            }
        }
    }

    // ---- (c) the events inside the window ---------------------------------
    const int64_t wa = std::max(startPos, e.startTime);
    const int64_t wb = std::min(winEnd, clipEnd);
    int64_t p = wa - e.startTime;
    const int64_t pEnd = wb - e.startTime;
    while (p < pEnd) {
        const twEventMapping m = applyMap(res.map, p);
        const int64_t segEndClip = (m.runFrames == INT64_MAX)
                                 ? INT64_MAX : p + m.runFrames;
        const int64_t segEndTrack = (segEndClip == INT64_MAX)
                                  ? INT64_MAX : e.startTime + segEndClip;
        const int64_t scanEnd = std::min(segEndClip, pEnd);
        const auto range = seq.slice(m.seqPos, m.seqPos + (scanEnd - p));
        for (size_t i = range.first; i < range.second; ++i) {
            const twEvent &src = seq.events()[i];
            if (twEventIsMetadata(src.kind)) continue;   // score, not performance
            if (src.flags & twEventMuted) continue;
            const int64_t tTrack = e.startTime + p + (src.time - m.seqPos);

            twEvent ev = src;
            ev.time = tTrack - startPos;
            ev.duration = 0;      // a collected note-on always gets its own off
            if (src.payloadSize) {
                const uint8_t *pl = seq.payload(src);
                ev.payloadOffset = out.addPayload(pl, pl ? src.payloadSize : 0);
                ev.payloadSize = pl ? src.payloadSize : 0;
            }

            switch (src.kind) {
            case twEventKind::NoteOn: {
                ev.noteId = twMakeNoteId(e.slot, (int64_t)i);
                out.events.push_back(ev);
                const int64_t endTrack = (src.duration > 0)
                    ? tTrack + src.duration : INT64_MAX;
                pushOpen(src.channel, src.key, ev.noteId, endTrack, segEndTrack);
                break;
            }
            case twEventKind::NoteOff:
            case twEventKind::NoteChoke:
            case twEventKind::NoteEnd: {
                ev.noteId = popOpen(src.channel, src.key);
                out.events.push_back(ev);
                break;
            }
            default:
                out.events.push_back(ev);
                break;
            }
        }
        if (scanEnd <= p) break;    // defensive: a pathological map
        p = scanEnd;
    }

    // ---- (d) what is still sounding: real ends and synthesised ones --------
    for (const OpenNote &o : open) {
        const int64_t stop = std::min(o.endTrack, o.boundaryTrack);
        if (stop == INT64_MAX) continue;          // still held past this window
        if (stop < startPos || stop >= winEnd) continue;
        twEvent off;
        off.time = stop - startPos;
        off.kind = twEventKind::NoteOff;
        off.flags = (stop < o.endTrack) ? twEventSynthesisedOff : twEventFlagNone;
        off.channel = o.channel; off.key = o.key;
        off.noteId = o.id;
        off.value = 0.0;
        out.events.push_back(off);
    }
}

void twEventClipSet::collect(int64_t startPos, int64_t len,
                             twEventBlock &out) const
{
    out.clear();
    if (len <= 0) return;
    std::vector<Entry> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = clips_;     // one coherent list for the whole call
    }
    for (const Entry &e : snapshot) collectClip_(e, startPos, len, out);
    out.chase.sortNotes();
    out.sortEvents();
}
