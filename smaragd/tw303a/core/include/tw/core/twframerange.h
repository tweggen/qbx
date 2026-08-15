#ifndef _TWFRAMERANGE_H_
#define _TWFRAMERANGE_H_

#include <cstdint>

/**
 * A half-open extent of timeline frames, [start, end).
 *
 * The value every position-scoped edit returns so the caller can stale only
 * the part of the downstream chain the edit can be heard in (proposal 18
 * Phase 5). tw/mix's twEditRange is the same shape for the audio clip list;
 * this one lives in core so a module that may not depend on tw/mix — tw/events,
 * because tw/plugins may not include tw/mix (proposal 36 F15) — can return the
 * same thing without inventing a second type.
 *
 * Signed, like offset_t: a range may start before zero (a clip anchored ahead
 * of its data). The "reaches the end of time" sentinel is INT64_MAX, never
 * UINT64_MAX — as unsigned that wraps to -1 and compares BELOW every real
 * position, which is how an unbounded range once degenerated to empty().
 */
struct twFrameRange {
    int64_t start = 0;
    int64_t end   = 0;

    bool empty() const { return end <= start; }

    void unite(int64_t s, int64_t e) {
        if (e <= s) return;
        if (empty()) { start = s; end = e; return; }
        if (s < start) start = s;
        if (e > end)   end = e;
    }
    void unite(const twFrameRange &o) {
        if (!o.empty()) unite(o.start, o.end);
    }

    bool operator==(const twFrameRange &o) const {
        if (empty() && o.empty()) return true;
        return start == o.start && end == o.end;
    }
    bool operator!=(const twFrameRange &o) const { return !(*this == o); }
};

#endif // _TWFRAMERANGE_H_
