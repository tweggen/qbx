#ifndef _TWDOMAINS_H_
#define _TWDOMAINS_H_

#include <cstdint>
#include "tw/core/twfraction.h"

/**
 * Typed time domains (proposal 18 Phase 1).
 *
 * Every position bug in this codebase has been a confusion between the
 * domains of docs/contracts/POSITION_DOMAINS.md, or a length transformed
 * like a point. These wrappers make both mistakes compile errors:
 *
 *   - Positions and lengths are distinct types per domain.
 *     Pos - Pos = Len, Pos ± Len = Pos, Len ± Len = Len.
 *     Pos + Pos does not compile; cross-domain arithmetic does not compile.
 *   - Conversions between domains are the NAMED functions at the bottom of
 *     this header - one implementation per conversion, nothing inline at
 *     call sites.
 *
 * The domains (see POSITION_DOMAINS.md):
 *   Timeline - frames @ project rate, zero at project start
 *   Clip     - frames @ project rate, zero at the clip's startTime
 *   Warped   - grain-OUTPUT (stretched) frames; the domain the cut window
 *              (startOffset/loopLength) is addressed in
 *   Source   - frames of the concrete source material (recording/capture)
 *   Tick     - musical pulses (PPQ), tempo- and rate-free; the domain
 *              MIDI content and an event clip's window live in
 *              (proposal 36 D2). Exact-rational, not int64 frames.
 *
 * The raw engine currency (offset_t/length_t, page keys, reader cursors)
 * stays untyped; these types live where windows are DEFINED and
 * TRANSFORMED (SCut, clip actions, gestures, renderers). Unwrapping via
 * .frames() marks the seam - and the seam is where rounding is allowed.
 */

template <class D>
struct DomainLen {
    constexpr DomainLen() : v_(0) {}
    constexpr explicit DomainLen(int64_t frames) : v_(frames) {}
    constexpr int64_t frames() const { return v_; }

    friend constexpr DomainLen operator+(DomainLen a, DomainLen b) { return DomainLen(a.v_ + b.v_); }
    friend constexpr DomainLen operator-(DomainLen a, DomainLen b) { return DomainLen(a.v_ - b.v_); }
    friend constexpr bool operator==(DomainLen a, DomainLen b) { return a.v_ == b.v_; }
    friend constexpr bool operator!=(DomainLen a, DomainLen b) { return a.v_ != b.v_; }
    friend constexpr bool operator<(DomainLen a, DomainLen b)  { return a.v_ < b.v_; }
    friend constexpr bool operator<=(DomainLen a, DomainLen b) { return a.v_ <= b.v_; }
    friend constexpr bool operator>(DomainLen a, DomainLen b)  { return a.v_ > b.v_; }
    friend constexpr bool operator>=(DomainLen a, DomainLen b) { return a.v_ >= b.v_; }

private:
    int64_t v_;
};

template <class D>
struct DomainPos {
    constexpr DomainPos() : v_(0) {}
    constexpr explicit DomainPos(int64_t frames) : v_(frames) {}
    constexpr int64_t frames() const { return v_; }

    friend constexpr DomainPos operator+(DomainPos p, DomainLen<D> l) { return DomainPos(p.v_ + l.frames()); }
    friend constexpr DomainPos operator-(DomainPos p, DomainLen<D> l) { return DomainPos(p.v_ - l.frames()); }
    friend constexpr DomainLen<D> operator-(DomainPos a, DomainPos b) { return DomainLen<D>(a.v_ - b.v_); }
    friend constexpr bool operator==(DomainPos a, DomainPos b) { return a.v_ == b.v_; }
    friend constexpr bool operator!=(DomainPos a, DomainPos b) { return a.v_ != b.v_; }
    friend constexpr bool operator<(DomainPos a, DomainPos b)  { return a.v_ < b.v_; }
    friend constexpr bool operator<=(DomainPos a, DomainPos b) { return a.v_ <= b.v_; }
    friend constexpr bool operator>(DomainPos a, DomainPos b)  { return a.v_ > b.v_; }
    friend constexpr bool operator>=(DomainPos a, DomainPos b) { return a.v_ >= b.v_; }

private:
    int64_t v_;
};

struct TimelineDomain {};
struct ClipDomain {};
struct WarpedDomain {};
struct SourceDomain {};

using TimelinePos = DomainPos<TimelineDomain>;
using TimelineLen = DomainLen<TimelineDomain>;
using ClipPos     = DomainPos<ClipDomain>;
using ClipLen     = DomainLen<ClipDomain>;
using WarpedPos   = DomainPos<WarpedDomain>;
using WarpedLen   = DomainLen<WarpedDomain>;
using SourcePos   = DomainPos<SourceDomain>;
using SourceLen   = DomainLen<SourceDomain>;

// ============================================================================
// Ticks - musical time (proposal 36 D2). The ONE domain here that is NOT
// frames: an event's position in a score, at a fixed pulses-per-quarter
// resolution, independent of tempo and of sample rate.
//
// EXACT-RATIONAL, unlike the four frame domains above, and for the same
// reason SCut's srcStart is: a tick position is produced by dividing (a
// timeline frame through the tempo map, a split point, a quantize grid), and
// rounding it at birth is exactly the drift D2 rejects. Content events carry
// INTEGER ticks (TickPos(int64_t)); the rational form exists so the arithmetic
// between them stays lossless.
//
// The ONLY converter between ticks and frames is twTempoMap (tw/events) -
// there is no ticksToFrames here, because the conversion needs the tempo,
// which is state, and core holds none.
// ============================================================================

struct TickDomain {};

struct TickLen {
    TickLen() : v_(0) {}
    explicit TickLen(int64_t ticks) : v_(ticks) {}
    explicit TickLen(const Fraction &ticks) : v_(ticks) {}
    const Fraction &ticks() const { return v_; }
    // Explicit, lossy projections - the seam where rounding is allowed.
    int64_t floorTicks() const { return v_.floorToInt(); }
    int64_t ceilTicks() const { return v_.ceilToInt(); }

    friend TickLen operator+(TickLen a, TickLen b) { return TickLen(a.v_ + b.v_); }
    friend TickLen operator-(TickLen a, TickLen b) { return TickLen(a.v_ - b.v_); }
    friend TickLen operator*(TickLen a, const Fraction &f) { return TickLen(a.v_ * f); }
    friend TickLen operator-(TickLen a) { return TickLen(-a.v_); }
    friend bool operator==(TickLen a, TickLen b) { return a.v_ == b.v_; }
    friend bool operator!=(TickLen a, TickLen b) { return a.v_ != b.v_; }
    friend bool operator<(TickLen a, TickLen b)  { return a.v_ < b.v_; }
    friend bool operator<=(TickLen a, TickLen b) { return a.v_ <= b.v_; }
    friend bool operator>(TickLen a, TickLen b)  { return a.v_ > b.v_; }
    friend bool operator>=(TickLen a, TickLen b) { return a.v_ >= b.v_; }

private:
    Fraction v_;
};

struct TickPos {
    TickPos() : v_(0) {}
    explicit TickPos(int64_t ticks) : v_(ticks) {}
    explicit TickPos(const Fraction &ticks) : v_(ticks) {}
    const Fraction &ticks() const { return v_; }
    int64_t floorTicks() const { return v_.floorToInt(); }
    int64_t ceilTicks() const { return v_.ceilToInt(); }

    friend TickPos operator+(TickPos p, TickLen l) { return TickPos(p.v_ + l.ticks()); }
    friend TickPos operator-(TickPos p, TickLen l) { return TickPos(p.v_ - l.ticks()); }
    friend TickLen operator-(TickPos a, TickPos b) { return TickLen(a.v_ - b.v_); }
    friend bool operator==(TickPos a, TickPos b) { return a.v_ == b.v_; }
    friend bool operator!=(TickPos a, TickPos b) { return a.v_ != b.v_; }
    friend bool operator<(TickPos a, TickPos b)  { return a.v_ < b.v_; }
    friend bool operator<=(TickPos a, TickPos b) { return a.v_ <= b.v_; }
    friend bool operator>(TickPos a, TickPos b)  { return a.v_ > b.v_; }
    friend bool operator>=(TickPos a, TickPos b) { return a.v_ >= b.v_; }

private:
    Fraction v_;
};

// ============================================================================
// Named domain conversions - THE single implementation of each mapping.
// (POSITION_DOMAINS.md rules 1-3; proposal 18 map inventory.)
// ============================================================================

// Timeline -> clip-relative: subtract the clip's placement (SLink startTime).
// The ShiftMap. Tracks speak clip-relative (rule 1).
inline ClipPos clipFromTimeline(TimelinePos p, TimelinePos clipStart) {
    return ClipPos(p.frames() - clipStart.frames());
}
inline TimelinePos timelineFromClip(ClipPos p, TimelinePos clipStart) {
    return TimelinePos(clipStart.frames() + p.frames());
}

// Clip-relative -> warped: fold in the cut's slip offset. The WindowMap.
// Scale 1: the warped domain IS timeline-rate frames, shifted (rule 3) -
// which is also why clip-domain LENGTHS carry over unchanged.
inline WarpedPos warpedFromClip(ClipPos p, WarpedPos startOffset) {
    return WarpedPos(startOffset.frames() + p.frames());
}
inline ClipPos clipFromWarped(WarpedPos p, WarpedPos startOffset) {
    return ClipPos(p.frames() - startOffset.frames());
}
inline WarpedLen warpedFromClip(ClipLen l) { return WarpedLen(l.frames()); }
inline ClipLen clipFromWarped(WarpedLen l) { return ClipLen(l.frames()); }

// Warped <-> source: through the stretch factor. The StretchMap.
// EXACT rational results; rounding (floorToInt) is the caller's explicit,
// render-boundary decision. stretch = output/input duration ratio, so
// source = warped / stretch and warped = source * stretch.
inline Fraction sourceFromWarped(WarpedPos p, const Fraction& stretch) {
    return Fraction(p.frames()) / stretch;
}
inline Fraction sourceFromWarped(WarpedLen l, const Fraction& stretch) {
    return Fraction(l.frames()) / stretch;
}
inline Fraction warpedFromSource(SourcePos p, const Fraction& stretch) {
    return Fraction(p.frames()) * stretch;
}
inline Fraction warpedFromSource(SourceLen l, const Fraction& stretch) {
    return Fraction(l.frames()) * stretch;
}

// Identity passthrough for the unstretched case (stretch == 1): warped and
// source coincide. Named so the call site says what it means.
inline SourcePos sourceIsWarpedIdentity(WarpedPos p) { return SourcePos(p.frames()); }
inline WarpedPos warpedIsSourceIdentity(SourcePos p) { return WarpedPos(p.frames()); }

#endif // _TWDOMAINS_H_
