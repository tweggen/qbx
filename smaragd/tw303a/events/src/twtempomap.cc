#include "tw/events/twtempomap.h"

#include <cmath>

void twTempoMap::setBpm(double bpm)
{
    if (!(bpm > 0.0)) return;
    const double us = 6.0e7 / bpm;
    // llround, not truncation: 6e7/120 is 500000 either way, but 6e7/113 is
    // 530973.45… and truncating biases every project slightly fast.
    int64_t v = (int64_t)std::llround(us);
    if (v < 1) v = 1;
    usPerQuarter_ = v;
}

Fraction twTempoMap::ticksToFrames(TickPos t, int sampleRate) const
{
    return ticksToFrames(TickLen(t.ticks()), sampleRate);
}

Fraction twTempoMap::ticksToFrames(TickLen l, int sampleRate) const
{
    if (sampleRate <= 0) return Fraction(0);
    // frames = ticks · us · srate / (ppq · 10^6). Built as ONE fraction so the
    // reduction happens once and the intermediate never leaves 128 bits.
    const Fraction scale(usPerQuarter_ * (int64_t)sampleRate,
                         (int64_t)ppq_ * 1000000LL);
    return l.ticks() * scale;
}

TickPos twTempoMap::framesToTicks(int64_t frames, int sampleRate) const
{
    return TickPos(framesToTickLen(frames, sampleRate).ticks());
}

TickLen twTempoMap::framesToTickLen(int64_t frames, int sampleRate) const
{
    if (sampleRate <= 0) return TickLen((int64_t)0);
    const Fraction scale((int64_t)ppq_ * 1000000LL,
                         usPerQuarter_ * (int64_t)sampleRate);
    return TickLen(Fraction(frames) * scale);
}

Fraction twTempoMap::quarterNoteFrames(int sampleRate) const
{
    return ticksToFrames(TickLen((int64_t)ppq_), sampleRate);
}

Fraction twTempoMap::barFrames(int sampleRate) const
{
    // A bar is numerator notes of 1/denominator each; a quarter is 1/4.
    const Fraction beats(numerator_ * 4, denominator_);
    return quarterNoteFrames(sampleRate) * beats;
}
