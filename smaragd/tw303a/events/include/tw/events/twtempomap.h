#ifndef _TW_TEMPOMAP_H_
#define _TW_TEMPOMAP_H_

#include <cstdint>

#include "tw/core/twdomains.h"
#include "tw/core/twfraction.h"

/**
 * twTempoMap — THE tick↔frame converter and the single tempo authority
 * (proposal 37 D2).
 *
 * Tempo is stored the way SMF stores it: MICROSECONDS PER QUARTER NOTE, an
 * integer. BPM is a DERIVED view (`bpm() == 6e7 / usPerQuarter`), never a
 * second source of truth — a project that stored 60/bpm seconds per beat and a
 * map that stored µs/quarter disagree in the tenth microsecond, and that
 * difference lands on a frame boundary in a long project.
 *
 * The conversion is therefore EXACT rational arithmetic:
 *
 *     frames = ticks · usPerQuarter · srate / (ppq · 10⁶)
 *
 * and the reduced denominator can never exceed ppq · 10⁶ / gcd(...) — decades
 * of frames away from Fraction's int64 red line (asserted in events_test).
 * Rounding to an integer frame is the CALLER's explicit decision at the render
 * boundary (`Fraction::floorToInt`), exactly as for the warp map.
 *
 * Constant tempo only, today. The API is shaped so tempo SEGMENTS (proposal 37)
 * are an implementation change here and nowhere else: everything positional
 * goes through `ticksToFrames`/`framesToTicks`, which already take a position
 * rather than a duration, and `usPerQuarterAt(TickPos)` already asks per
 * position. Nobody outside may compute a tick from a frame by multiplying.
 *
 * Plain value type: copyable, no locking, no Qt. The app holds one per project
 * and swaps it under its own mutex like every other snapshot.
 */
class twTempoMap
{
public:
    static constexpr int32_t DEFAULT_PPQ = 960;      // D2: the house resolution
    static constexpr int64_t DEFAULT_US_PER_QUARTER = 500000;  // 120 BPM

    twTempoMap() = default;
    twTempoMap(int64_t usPerQuarter, int32_t ppq)
        : usPerQuarter_(usPerQuarter > 0 ? usPerQuarter : DEFAULT_US_PER_QUARTER),
          ppq_(ppq > 0 ? ppq : DEFAULT_PPQ) {}

    int64_t usPerQuarter() const { return usPerQuarter_; }
    void setUsPerQuarter(int64_t us) { if (us > 0) usPerQuarter_ = us; }
    // Per-position accessor — the seam tempo segments will grow behind.
    int64_t usPerQuarterAt(TickPos) const { return usPerQuarter_; }

    int32_t ppq() const { return ppq_; }
    // Changing the resolution does NOT move anything: it is the unit the
    // caller's tick values are expressed in. Rescaling the CONTENT is
    // twSmf::rescalePpq's job.
    void setPpq(int32_t ppq) { if (ppq > 0) ppq_ = ppq; }

    int32_t numerator() const { return numerator_; }
    int32_t denominator() const { return denominator_; }
    void setTimeSignature(int32_t num, int32_t den) {
        if (num > 0) numerator_ = num;
        if (den > 0) denominator_ = den;
    }

    // DERIVED. Never stored — see the class comment.
    double bpm() const { return 6.0e7 / (double)usPerQuarter_; }
    // Stores round(6e7 / bpm), which is what `set-tempo bpm=` writes.
    void setBpm(double bpm);

    // EXACT. The caller rounds (floorToInt) at the render boundary.
    Fraction ticksToFrames(TickPos t, int sampleRate) const;
    Fraction ticksToFrames(TickLen l, int sampleRate) const;
    TickPos  framesToTicks(int64_t frames, int sampleRate) const;
    TickLen  framesToTickLen(int64_t frames, int sampleRate) const;

    // Frames per quarter note / per bar at this rate, exact.
    Fraction quarterNoteFrames(int sampleRate) const;
    Fraction barFrames(int sampleRate) const;

    bool operator==(const twTempoMap &o) const {
        return usPerQuarter_ == o.usPerQuarter_ && ppq_ == o.ppq_
            && numerator_ == o.numerator_ && denominator_ == o.denominator_;
    }
    bool operator!=(const twTempoMap &o) const { return !(*this == o); }

private:
    int64_t usPerQuarter_ = DEFAULT_US_PER_QUARTER;
    int32_t ppq_ = DEFAULT_PPQ;
    int32_t numerator_ = 4;
    int32_t denominator_ = 4;
};

#endif // _TW_TEMPOMAP_H_
