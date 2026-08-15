#ifndef _TW_EVENTMERGE_H_
#define _TW_EVENTMERGE_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "tw/events/tweventsource.h"

/**
 * twEventMerge - one twEventSource over N of them (proposal 36 §3.2.1).
 *
 * This is a track's FEED: its own event clip set merged with the feeds of every
 * child track that passes events up ("consumed here, or bubbled up"). A folder
 * with a drum machine on it plays its children's patterns because its feed IS
 * this merge; the instrument slot and the MIDI-out pump cannot tell a merge
 * from a plain clip set.
 *
 * Three rules, all of them load-bearing:
 *   - k-way MERGE BY TIME, so the result is one sorted stream (a consumer that
 *     received two concatenated streams would send a plugin an unsorted list,
 *     which every format forbids);
 *   - the chase set is the UNION of the sources' chases, folded in source
 *     order, so a controller two sources both wrote resolves to the LAST
 *     source's value deterministically;
 *   - note ids are NAMESPACED PER SOURCE (the source index in the high bits,
 *     twNamespaceNoteId). Two children playing the same key on the same channel
 *     are then two overlapping notes to the instrument, never one note that the
 *     first note-off truncates.
 *
 * Sources are held by shared_ptr and the list is copied under the mutex for the
 * duration of a collect, so a source REMOVED mid-stream is simply absent from
 * the next collect - it cannot be destroyed underneath one in flight.
 */
class twEventMerge : public twEventSource
{
public:
    twEventMerge() = default;

    // The index a source gets here is its note-id namespace, so ORDER MATTERS:
    // rebuilding the feed with the sources in a different order renumbers ids
    // (harmless - ids only have to be distinct within one collect - but it is
    // why the app rebuilds the whole feed rather than patching it).
    void setSources(std::vector<std::shared_ptr<const twEventSource>> sources);
    void addSource(std::shared_ptr<const twEventSource> source);
    bool removeSource(const twEventSource *source);
    void clear();
    size_t sourceCount() const;

    void collect(int64_t startPos, int64_t len, twEventBlock &out) const override;

private:
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<const twEventSource>> sources_;
};

#endif // _TW_EVENTMERGE_H_
