#ifndef _TWGROOVEMETRICS_H_
#define _TWGROOVEMETRICS_H_

#include <cstdint>
#include <string>
#include <vector>

#include "tw/sidecar/twgrooveaspect.h"

/**
 * Proposal 40 "Feel Flow" M3b -- READ-SIDE metric derivation: the metric
 * lab's per-hop series, computed from the two aspects already on disk
 * ("groove.res" / "groove.ev", decoded by twgrooveaspect.h) with NO analyzer
 * change, NO aspect version bump and NO re-analysis. Pure, deterministic,
 * single-threaded: no engine state, no I/O, no Qt, no threads, no rand().
 *
 * Why this module exists (the M3b motivation, stated where the code is):
 * the shipped heatmap tint is twGroovePendulumResult::confidence -- the
 * reference unit's resonance power alone, section 3.3's R(t) backbone --
 * which is monotone in drive energy and event DENSITY, so dense material
 * paints green whether or not it supports the entrained oscillation. The
 * residual data that actually encodes support-vs-disturb sits in
 * "groove.ev" and never reached the tint. Rather than swap one scalar for
 * another by fiat, this module derives a DOZEN-plus candidate series so the
 * requester can judge them side by side -- including an explicit `density`
 * row, labeled as such, so every other metric can be judged against the
 * confound by eye.
 *
 * Every series value is in [0,1], LUT-ready (1 = green / "supports the
 * established feel", 0 = red), EXCEPT the no-data sentinel: a value < 0
 * means "not enough events in this window to say anything" and a consumer
 * must paint it NEUTRAL (lane-fill grey), never red -- proposal 40 trap 9's
 * fill/break rule made structural: a break is not a compliance failure, it
 * is an absence of evidence.
 */

/**
 * The section 2.3 READ-SIDE constants (JND floor, feel band, fusion
 * ceiling) plus the windowing this module adds. Defaults are the
 * literature anchors quoted in the design; every one is a read-time
 * parameter -- changing one re-derives the series from the SAME decoded
 * payloads, never re-keys or re-runs the analysis.
 */
struct twGrooveReadParams {
    // Friberg & Sundberg 1995: differences below ~6 ms are not
    // discriminable; sigma at or under this maps to 1.0 (never penalized).
    double jndFloorMs = 6.0;
    // The stable-offset cluster of section 2.2 (~10-30 ms): sigma at or
    // past this maps to 0.0, and a local-mu excursion of this size maps
    // the mudrift series to 0.0.
    double feelBandMs = 30.0;
    // Danielsen/London: past ~40 ms two fast-attack sounds segregate --
    // "compliance" is the wrong category. Events past the ceiling are
    // EXCLUDED from sigma/mu statistics and counted by the `outliers`
    // series instead.
    double fusionCeilingMs = 40.0;

    // Windows, in seconds of analyzed material. sigmaWindowSec bounds the
    // local jitter estimate (short: a couple of bars); muWindowSec bounds
    // the local-mean estimate the mudrift series compares against the
    // whole-run mu (longer: phrase scale); rollingNormSec is the rolling
    // peak window the `rollnorm` series re-normalizes compliance by.
    double sigmaWindowSec  = 3.0;
    double muWindowSec     = 8.0;
    double rollingNormSec  = 8.0;

    // Fewer in-category events than this in a window -> the sentinel, not
    // a value: robust statistics over 2 points are noise wearing a number.
    uint32_t minWindowEvents = 4;

    // --- Tier B ("groove.dyn", proposal 40 M3c) read-side constants -------
    // dynSmoothSec: centered moving-average window over the raw per-hop
    // support/tension series (impulsive by nature -- the drive IS the
    // rectified flux) before display normalization. slipCap: a windowed
    // mean slip at or past this maps the `slip` series to 0.0 (red).
    // leanWindowSec: the window for the F-weighted lean.
    double dynSmoothSec  = 1.0;
    double slipCap       = 0.25;
    double leanWindowSec = 8.0;
};

/** One derived per-hop series. `id` is the stable, script-addressable
 * spelling (docs/ACTIONS.md and the qxa gates use it); `label` is the
 * human one the panel rows show. value.size() == the res-record count;
 * each value in [0,1], or < 0 = the no-data sentinel (see above). */
struct twGrooveMetricSeries {
    std::string        id;
    std::string        label;
    std::vector<float> value;
};

/**
 * Derives the full M3b series set from one decoded aspect pair. Series
 * order is stable and contractual (the panel rows and the describe()
 * grammar both follow it):
 *
 *   compliance        the existing scalar, verbatim -- the CONTROL row
 *   power:<unit>      per-unit normalized resonance power (one row per
 *                     ensemble unit; unitNames[u] when provided, else
 *                     "unit<u>")
 *   rollnorm          compliance re-normalized by a centered rolling-window
 *                     peak (rollingNormSec) instead of the whole-run peak --
 *                     isolates the run-peak half of the density coupling
 *   sigma             windowed robust residual spread (1.4826*MAD around
 *                     the window median, so it is drift-corrected at window
 *                     scale), mapped: sigma <= jndFloorMs -> 1.0, sigma >=
 *                     feelBandMs -> 0.0, linear between. In-category events
 *                     only (|residual| <= fusionCeilingMs).
 *   mudrift           1 - |local mu - global mu| / feelBandMs (clamped),
 *                     local over muWindowSec, both medians of in-category
 *                     residuals -- "the feel is moving", not "the feel is
 *                     wrong": a stable lean scores 1.0 wherever it holds
 *   outliers          1 - (events past the fusion ceiling / all events in
 *                     the sigma window)
 *   evconf            windowed mean event confidence
 *   score             the section 3.3 composite this lab can honestly
 *                     compute read-side: compliance * sigma-penalty (the
 *                     sigma series' mapped value; 1.0 where sigma has no
 *                     data, so a break falls back to the compliance dip
 *                     rather than doubling it). Region weighting is
 *                     deliberately NOT applied here (Tier B).
 *   density           windowed event count / the run's max windowed count
 *                     -- THE CONFOUND, included on purpose and labeled so
 *
 * TIER B (proposal 40 M3c), appended AFTER the series above and PRESENT
 * ONLY when dyn records were supplied and match the res-record count -- a
 * pre-M3c store (res+ev, no dyn) shows exactly the 13 Tier A series, never
 * sentinel-filled ghost rows:
 *
 *   support           the reference unit's signed in-phase drive, smoothed
 *                     (centered mean over dynSmoothSec) and normalized by
 *                     the run peak |smoothed|, mapped 0.5-CENTERED: 0.5 =
 *                     neutral, green (> 0.5) = the impulse pushes the
 *                     established oscillation, red (< 0.5) = it brakes it.
 *                     THE direct answer to the swing question.
 *   tension           the quadrature twin (section 3.5's c_p): green/red =
 *                     sustained force shoving the phase ahead/behind of the
 *                     settling point -- the "held lean", per hop.
 *   lean              windowed F-WEIGHTED mean of sin(phi) = sum(tension) /
 *                     sum(hypot(support,tension)) over leanWindowSec --
 *                     the section 3.5 loudness-confound separation done by
 *                     construction (a crescendo cannot move it), signed in
 *                     [-1,1], mapped 0.5-centered. Sentinel where the
 *                     window's total drive is ~0.
 *   slip              the reference's windowed mean phase-velocity
 *                     deviation, mapped 1 - clamp(slip/slipCap): green =
 *                     locked, red = the swing is being knocked off its
 *                     rate.
 *   move:<unit>       per-unit dissipated power over that unit's own run
 *                     peak -- section 3.4's per-body-part "predicted
 *                     movement energy" (bounce/sway/limbs/...), [0,1].
 *
 * Returns an empty vector when res is empty or hopFrames/rate is 0.
 * ev may legitimately be empty (a run with no scored events): the
 * res-derived series still come back and every event-derived hop holds the
 * sentinel (density holds 0.0 -- zero events is a real density).
 */
std::vector<twGrooveMetricSeries> twGrooveDeriveMetrics(
    const std::vector<twGrooveResRecord> &res,
    const std::vector<twGrooveEvRecord>  &ev,
    const std::vector<twGrooveDynRecord> &dyn,
    uint32_t hopFrames, uint32_t rate,
    const std::vector<std::string> &unitNames,
    const twGrooveReadParams &params );

/** Tier-A-only convenience overload (no dyn records) -- byte-identical to
 * passing an empty dyn vector. */
std::vector<twGrooveMetricSeries> twGrooveDeriveMetrics(
    const std::vector<twGrooveResRecord> &res,
    const std::vector<twGrooveEvRecord>  &ev,
    uint32_t hopFrames, uint32_t rate,
    const std::vector<std::string> &unitNames,
    const twGrooveReadParams &params );

#endif
