// asio_bufsize — turn ASIOGetBufferSize's four numbers into a list a combo box
// can show.
//
// PRIVATE header (devices/src/), SDK-free and pure, so `multi_backend_test`
// can assert the walk on any platform without a driver. Proposal 35, Phase 2.
//
// Proposal 35's Files table puts this walk inside `asio_device`; it is a free
// function in its own header instead, for the reason the design gives for
// `asio_id` and `asio_convert`: the parts that can be gated without hardware
// should be, and this one is pure arithmetic with three branches that are easy
// to get subtly wrong and impossible to notice from a device that only offers
// one size (measured: the Tascam US-16x08 reports min == max == preferred ==
// 256, granularity 0 — so it exercises exactly one of the three).
//
// ASIO's contract for `granularity`:
//
//     -1  -> sizes are POWERS OF TWO from min to max
//      0  -> the buffer size is FIXED; only `preferred` is selectable
//     >0  -> min, min+g, min+2g, ... up to max
//
// The result always CONTAINS `preferred`, is ascending, and is deduplicated.
// It is capped so a driver with granularity 1 and a wide range cannot produce
// a 100 000-entry combo box; the cap keeps the entries nearest `preferred`,
// because that is the value a user is choosing around.

#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace audio {

// The most entries a picker should ever show. Not a device limit — a UI one.
inline constexpr std::size_t kAsioMaxBufferChoices = 32;

inline std::vector<std::uint32_t> asioBufferSizeCandidates(long minSize, long maxSize,
                                                           long preferred, long granularity)
{
    std::vector<std::uint32_t> out;

    // A driver that reports nonsense gets the one answer that is always safe:
    // the size it says it prefers. Returning an empty list here would be read
    // upstream as "not user-selectable", which is also true, but losing the
    // preferred value loses the only number we know is valid.
    if (preferred <= 0) {
        if (minSize > 0) out.push_back((std::uint32_t) minSize);
        return out;
    }
    if (minSize <= 0 || maxSize < minSize) {
        out.push_back((std::uint32_t) preferred);
        return out;
    }

    if (granularity == 0 || minSize == maxSize) {
        // FIXED. The size lives in the driver's own control panel, so the app
        // has exactly one thing to show and `setBufferSize` has nothing to do.
        out.push_back((std::uint32_t) preferred);
        return out;
    }

    if (granularity < 0) {
        // POWERS OF TWO. Start at the first power of two >= min; a driver may
        // report a min that is not itself one.
        long p = 1;
        while (p < minSize) p <<= 1;
        for (; p <= maxSize && out.size() < 4096; p <<= 1)
            out.push_back((std::uint32_t) p);
    } else {
        for (long s = minSize; s <= maxSize && out.size() < 4096; s += granularity)
            out.push_back((std::uint32_t) s);
    }

    // `preferred` must be offerable even when the walk does not land on it —
    // a driver may prefer a size outside its own arithmetic series, and the
    // one value guaranteed to work must never be the one we cannot select.
    if (std::find(out.begin(), out.end(), (std::uint32_t) preferred) == out.end())
        out.push_back((std::uint32_t) preferred);

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());

    if (out.size() > kAsioMaxBufferChoices) {
        // Keep the window of entries around `preferred` rather than the first
        // N: the head of a granularity-1 series is a list of sizes nobody
        // wants, and the value the user is adjusting away from is the anchor.
        auto it = std::find(out.begin(), out.end(), (std::uint32_t) preferred);
        std::size_t idx = (it == out.end()) ? 0 : (std::size_t) (it - out.begin());
        std::size_t half = kAsioMaxBufferChoices / 2;
        std::size_t first = (idx > half) ? idx - half : 0;
        if (first + kAsioMaxBufferChoices > out.size())
            first = out.size() - kAsioMaxBufferChoices;
        out = std::vector<std::uint32_t>(out.begin() + (std::ptrdiff_t) first,
                                         out.begin() + (std::ptrdiff_t) (first + kAsioMaxBufferChoices));
    }
    return out;
}

// The size actually used for a request, honouring the same three rules. A
// driver rejects a size it did not offer, so the request is snapped rather
// than passed through: `setBufferSize` promises "the backend may return a
// different size than requested" and this is where that happens.
inline std::uint32_t asioSnapBufferSize(std::uint32_t want, long minSize, long maxSize,
                                        long preferred, long granularity)
{
    const std::vector<std::uint32_t> cands =
        asioBufferSizeCandidates(minSize, maxSize, preferred, granularity);
    if (cands.empty()) return (std::uint32_t) (preferred > 0 ? preferred : 0);

    std::uint32_t best = cands.front();
    std::uint64_t bestD = (std::uint64_t) (want > best ? want - best : best - want);
    for (std::uint32_t c : cands) {
        const std::uint64_t d = (std::uint64_t) (want > c ? want - c : c - want);
        if (d < bestD) { bestD = d; best = c; }
    }
    return best;
}

}  // namespace audio
