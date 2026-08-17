#ifndef _TW_EVENTCLIPSET_H_
#define _TW_EVENTCLIPSET_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "tw/core/twframerange.h"
#include "tw/events/tweventseq.h"
#include "tw/events/tweventsource.h"

/**
 * twEventClipSet — the event twin of twTrackMix's ClipEntry list (proposal 37
 * §4.2).
 *
 * NOT a twComponent, for the same reason the plugin slot processor is not: it
 * produces no pages. It is the per-track list of event clips — placement,
 * window, mute — and it answers `collect()` for the track.
 *
 * The rules it mirrors from tw/mix (CLIP_MODEL.md, mix/CONTRACT.md inv. 1-3):
 *   - identity is the caller's OPAQUE key (the app passes the SLink*), never
 *     the resolved sequence — two clips of one sequence share it;
 *   - clips are handed CLIP-RELATIVE positions and the resolver's map does the
 *     domain translation (slip, loop), exactly as twView's does for audio;
 *   - content is clamped to the clip window — the event twin of the clip-end
 *     clamp is a SYNTHESISED note-off (flag twEventSynthesisedOff) at the clip
 *     end for every note still held there;
 *   - every mutator returns the timeline extent it affected, so the app's
 *     invalidation walk can scope itself (proposal 18 Phase 5). It returns a
 *     core twFrameRange, not tw/mix's twEditRange: tw/plugins may not include
 *     tw/mix (F15), and the instrument slot is a consumer of this class.
 */

// The result of mapping one clip-relative position. `runFrames` is what a POINT
// map cannot give and enumeration cannot do without: the number of frames for
// which the mapping stays affine with slope 1 — i.e. the distance to the next
// loop wrap. INT64_MAX means "never breaks". (twTrackMix never needed it: it
// asks the clip to render a page and the clip loops internally. Events must be
// ENUMERATED, and an enumeration needs the segment, not just the point.)
struct twEventMapping {
    int64_t seqPos = 0;
    int64_t runFrames = INT64_MAX;
};

// clipPos (frames, zero at the clip's startTime) -> position in the sequence's
// own domain. Null = identity.
using twEventMapPosFn = std::function<twEventMapping(int64_t clipPos)>;

// This module's OWN resolved-clip record. Deliberately not tw/graph's
// twResolvedClip: tw/events is core-only and has no twComponent.
struct twEventClipResolved {
    std::shared_ptr<const twEventSeq> seq;
    twEventMapPosFn                   map;   // null = identity
};

// The two maps the app needs today, so every caller spells the same semantics.
// Slip: seqPos = clipPos + startOffset, never breaks.
twEventMapPosFn twEventSlipMap(int64_t startOffset);
// Loop: seqPos = startOffset + (clipPos mod loopLength); breaks every wrap.
twEventMapPosFn twEventLoopMap(int64_t startOffset, int64_t loopLength);

class twEventClipSet : public twEventSource
{
public:
    // Resolved ONCE per collect() per clip, at the window start — one snapshot
    // for the whole call, the same coherence rule twView::resolve gives the
    // audio path (proposal 19 Inv-1).
    using ResolveFn = std::function<twEventClipResolved(int64_t clipPos)>;

    twEventClipSet() = default;

    // duration <= 0 means UNBOUNDED (mirrors ClipEntry::duration == 0).
    twFrameRange insertClip(const void *key, int64_t startTime, int64_t duration,
                            ResolveFn resolve);
    twFrameRange updateClip(const void *key, int64_t startTime, int64_t duration);
    twFrameRange removeClip(const void *key);
    twFrameRange setClipMuted(const void *key, bool muted);
    // The content changed without the placement changing (a note edit).
    twFrameRange touchClip(const void *key);

    bool   hasClip(const void *key) const;
    size_t clipCount() const;
    void   clear();

    void collect(int64_t startPos, int64_t len, twEventBlock &out) const override;

private:
    struct Entry {
        const void *key = nullptr;
        int64_t     startTime = 0;
        int64_t     duration = 0;    // <= 0: unbounded
        bool        muted = false;
        int32_t     slot = 0;        // note-id namespace inside this set
        ResolveFn   resolve;
    };

    // Callers hold mutex_.
    const Entry *find_(const void *key) const;
    Entry       *find_(const void *key);
    static twFrameRange extentOf_(const Entry &e);
    void collectClip_(const Entry &e, int64_t startPos, int64_t len,
                      twEventBlock &out) const;

    mutable std::mutex mutex_;
    std::vector<Entry> clips_;
    int32_t            nextSlot_ = 0;
};

#endif // _TW_EVENTCLIPSET_H_
