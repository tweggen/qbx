#include "tw/events/tweventmerge.h"

#include <algorithm>

#include "tw/core/twlog.h"

void twEventMerge::setSources(std::vector<std::shared_ptr<const twEventSource>> sources)
{
    std::lock_guard<std::mutex> lock(mutex_);
    sources_ = std::move(sources);
    if (sources_.size() > (size_t)(TW_NOTEID_SOURCE_MASK + 1)) {
        // Beyond 16 sources the namespace wraps and two children CAN collide on
        // one id. Loud, once, and not fatal: the audible consequence is one
        // truncated note, not silence.
        TW_LOGW("events", "twEventMerge: %d sources exceeds the %d-source note-id "
                          "namespace; ids will alias",
                (int)sources_.size(), TW_NOTEID_SOURCE_MASK + 1);
    }
}

void twEventMerge::addSource(std::shared_ptr<const twEventSource> source)
{
    if (!source) return;
    std::vector<std::shared_ptr<const twEventSource>> next;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        next = sources_;
    }
    next.push_back(std::move(source));
    setSources(std::move(next));
}

bool twEventMerge::removeSource(const twEventSource *source)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < sources_.size(); ++i) {
        if (sources_[i].get() == source) {
            sources_.erase(sources_.begin() + (long)i);
            return true;
        }
    }
    return false;
}

void twEventMerge::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    sources_.clear();
}

size_t twEventMerge::sourceCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return sources_.size();
}

void twEventMerge::collect(int64_t startPos, int64_t len, twEventBlock &out) const
{
    out.clear();
    if (len <= 0) return;

    std::vector<std::shared_ptr<const twEventSource>> sources;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sources = sources_;    // pinned for the whole call: a source removed
                               // now cannot be destroyed under us
    }

    twEventBlock part;
    for (size_t s = 0; s < sources.size(); ++s) {
        if (!sources[s]) continue;
        part.clear();
        sources[s]->collect(startPos, len, part);

        const int32_t ns = (int32_t)s;
        for (twHeldNote &h : part.chase.notes) h.noteId = twNamespaceNoteId(h.noteId, ns);
        out.chase.mergeOver(part.chase);

        for (twEvent &e : part.events) {
            e.noteId = twNamespaceNoteId(e.noteId, ns);
            if (e.payloadSize) {
                const uint8_t *pl = part.payload(e);
                e.payloadOffset = out.addPayload(pl, pl ? e.payloadSize : 0);
                e.payloadSize = pl ? e.payloadSize : 0;
            }
            out.events.push_back(e);
        }
    }

    // The k-way merge itself. The parts are each already sorted, so a stable
    // sort of the concatenation IS the merge, and it keeps source order for
    // events that land on the same frame with the same rank.
    out.chase.sortNotes();
    out.sortEvents();
}
