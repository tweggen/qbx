
#ifndef _TWGROOVE_H_
#define _TWGROOVE_H_

#include <cstdint>
#include <vector>

/**
 * Groove analysis (proposal 40 "Feel Flow", M0). Pure, deterministic,
 * single-threaded functions over resident PCM -- no engine state, no I/O, no
 * Qt, no threads, no thread_local, no rand(). Mirrors the house convention of
 * twanalyzers.h: identical input and params produce identical output on a
 * given platform (fixed evaluation order).
 *
 * This file has TWO layers:
 *  - The excitation FRONT END (section 3.1): the cochlea-shaped IIR bank, its
 *    group-delay calibration, region pooling and a per-region event picker.
 *    `twGrooveAnalyzeFrontEnd()` and everything above `twGrooveRegionStats`.
 *  - The BASELINE estimator (M0 section 6 spec): a deliberately dumb
 *    "band-region onset picking + a local pulse fit + median/MAD residual
 *    statistics" second opinion the pendulum core (twgroovependulum.h) has to
 *    beat on the M0 fixture set (plan/proposed/40_GROOVE_RESONANCE.md section
 *    6, "review 5.3" -- the pendulum earns its complexity by measurement, not
 *    by assumption). `twGrooveBaseline*` below.
 *
 * Both consume ONLY this front end's twGrooveField -- no grid, no tempo map,
 * matching proposal 40 section 3's "the reference is not a quantization
 * grid" refinement.
 */

/**
 * Configuration for twGrooveAnalyzeFrontEnd(). fMaxHz is clamped internally to
 * stay comfortably below Nyquist (0.49 * rate) -- a caller need not do it, but
 * a value already below that margin is left untouched.
 */
struct twGrooveFrontEndParams {
    uint32_t nBands   = 64;        // gammatone-approximation bands, ERB-spaced
    float    fMinHz   = 40.0f;
    float    fMaxHz   = 16000.0f;  // clamped below Nyquist internally
    float    envRateHz = 200.0f;   // envelope/flux/event sample rate
    uint32_t nRegions = 10;        // log-spaced pooling regions over [fMinHz,fMaxHz]

    // Event-picking knobs (per region, over the region's flux curve). Mirrors
    // twDetectOnsets' median-adaptive-threshold convention (twanalyzers.h):
    // thr[k] = thresholdFactor * median(flux over +/- medianHalfWidth) +
    //          energyFloorFraction * (region's peak flux).
    float medianHalfWidthSec  = 0.5f;
    float thresholdFactor     = 1.5f;
    float energyFloorFraction = 0.05f;
    float minSeparationSec    = 0.03f;   // ~30 ms, the fusion-ceiling neighbourhood
};

/**
 * One picked event (a region-local envelope/flux maximum). Ascending by
 * posFrames within the returned twGrooveField::events. Position is in SOURCE
 * frames (the sample rate the analyzed buffer was supplied at), sub-frame via
 * parabolic interpolation over the flux peak.
 */
struct twGrooveEvent {
    double   posFrames = 0.0;
    float    amp        = 0.0f;   // interpolated flux value at the peak
    uint16_t region      = 0;      // index into twGrooveField::regionLowHz/HighHz

    bool operator==( const twGrooveEvent &o ) const {
        return posFrames == o.posFrames && amp == o.amp && region == o.region;
    }
};

/**
 * Output of the front end: the per-region envelope/flux field plus the picked
 * events. regionEnvelope/regionFlux are region-major, hop-minor:
 * value(r,k) = regionEnvelope[ r * nHops + k ].
 *
 * A region with no member band (nRegions too fine relative to nBands, or a
 * region whose interval falls entirely between two ERB-spaced band centers)
 * reads all zero for both arrays and contributes no events -- never garbage.
 */
struct twGrooveField {
    uint32_t rate      = 0;
    uint32_t hopFrames = 0;   // source frames per envelope/flux/event hop
    uint32_t nHops     = 0;

    std::vector<float> bandCenterHz;    // size nBands, ERB-spaced ascending
    std::vector<float> regionLowHz;     // size nRegions, log-spaced edges
    std::vector<float> regionHighHz;    // size nRegions

    std::vector<float> regionEnvelope;  // size nRegions * nHops
    std::vector<float> regionFlux;      // size nRegions * nHops

    std::vector<twGrooveEvent> events;  // ascending by posFrames
};

/**
 * Runs the front end (proposal 40 section 3.1) over planar SOURCE-rate
 * channels, folded to mono by arithmetic mean (matching twDetectOnsets /
 * twComputeLoudness). Steps, normative for this function:
 *
 *  1. mono[i] = mean over channels of chans[c][i].
 *  2. nBands ERB-spaced gammatone-approximation bands over
 *     [fMinHz, min(fMaxHz, 0.49*rate)]: per band, 4 cascaded complex one-pole
 *     filters y[n] = x[n] + a*y[n-1] with a = exp(-2*pi*b/rate) *
 *     exp(i*2*pi*fc/rate), b = 1.019*ERB(fc), ERB(f) = 24.7*(4.37*f/1000+1).
 *     The real input is doubled ONLY into the first cascade stage (the
 *     standard real-to-analytic compensation for a complex resonator driven
 *     by a real signal -- stages 2-4 receive the already near-single-sided
 *     output of stage 1 and are not doubled again); the envelope is
 *     |y4[n]| * gain, gain = (1-|a|)^4, which is what makes a unit-amplitude
 *     sine AT fc settle to envelope ~= 1 (the selectivity gate).
 *  3. Group-delay compensation (load-bearing, proposal 40 section 3.1): each
 *     band's raw envelope is advanced by delaySec = 3/(2*pi*b) BEFORE
 *     compression or decimation -- integer-sample shift plus linear
 *     interpolation of the fractional remainder, edge-holding past either
 *     end of the buffer.
 *  4. Compression: compressed = log1p(k*e) / log1p(k) (monotone, e>=0). k is
 *     a small constant (see twgroove.cc's compressSample -- deliberately
 *     milder than a "100"-scale textbook loudness compressor: at k=100 the
 *     region-pooled flux picked up a several-ms bias the group-delay term
 *     cannot remove, measured building the calibration gate).
 *  5. Decimation to envRateHz by MAX over each native-rate hop window
 *     (preserves transient timing; hopFrames = round(rate/envRateHz)).
 *  6. Region pooling: nRegions log-spaced regions over [fMinHz, effFMax]; a
 *     band belongs to the region its center falls in
 *     (floor(nRegions * ln(fc/fMinHz) / ln(effFMax/fMinHz)), clamped). Region
 *     envelope = mean of member bands' decimated-compressed envelopes.
 *     Region flux = half-wave-rectified first difference of the region
 *     envelope (flux[0] = 0).
 *  7. Event picking per region on the flux: median-adaptive threshold (see
 *     twGrooveFrontEndParams), local-maximum picking with minSeparationSec,
 *     parabolic interpolation over the peak for a fractional-hop position,
 *     PLUS a flux-lead correction of sqrt(3)/(2*pi*b) added to every event
 *     in the region (b = that region's own mean band bandwidth). This is a
 *     second, independently-derived closed form, not a fudge factor: flux
 *     is the DERIVATIVE of the (already delay-compensated) envelope, and for
 *     the gammatone-approximation impulse response shape a derivative peaks
 *     strictly BEFORE the envelope's own peak (which step 3's delaySec
 *     aligns to zero) -- by exactly this amount. Skipping it reproduces the
 *     same low-frequency-worst skew step 3 exists to remove, just smaller;
 *     see twgroove.cc's "Flux-lead correction" comment for the derivation.
 *     All regions' events are merged and sorted ascending by posFrames.
 *
 * Returns a default-constructed (all-empty) twGrooveField on invalid input:
 * chans == nullptr, nCh == 0, nFrames == 0, rate == 0, nBands == 0,
 * nRegions == 0, !(fMinHz > 0), !(fMaxHz > fMinHz), !(envRateHz > 0), or the
 * post-clamp effective fMax not exceeding fMinHz.
 */
twGrooveField twGrooveAnalyzeFrontEnd( const float *const *chans, uint32_t nCh,
                                       uint64_t nFrames, uint32_t rate,
                                       const twGrooveFrontEndParams &params );

// =============================================================================
// Shared residual/region statistics -- used by BOTH the baseline estimator
// below and the pendulum core (twgroovependulum.h), so a caller comparing the
// two reads the same shape from either. Proposal 40 section 3.3.
// =============================================================================

/**
 * One windowed sample of a region's local mu (the "mu-drift" rider of
 * proposal 40 section 3.3: slow feel modulation shows up here, not in sigma).
 */
struct twGrooveDriftSample {
    double tSec = 0.0;   // window center, seconds from analysis start
    double muMs = 0.0;
};

/**
 * Per-region circular-aware residual statistics. muMs is ROBUST (median),
 * never a plain mean -- a handful of spurious events must not swing "the
 * feel" the way they would swing a mean (proposal 40 section 3.3, and AC
 * (g)'s wash fixture is exactly the adversarial case).
 *
 * sigmaMs is the SPREAD AROUND THE LOCAL FEEL, not around the global median
 * (added in a second M0 iteration, coordinator-directed): proposal 40
 * section 3.3 defines mu-drift and sigma as SEPARATE readouts -- sigma is
 * jitter around the LOCAL trend, and a tempo ramp's slowly-varying residual
 * mean must not itself read as jitter. Computed as
 * 1.4826 * median(|residual_i - localMu(t_i)|), where localMu(t_i) is the
 * nearest sample of the SAME drift trace stored below (the windows are built
 * once and shared, so "how local is local" is one parameter, not two).
 * Falls back to 1.4826*median(|residual_i - muMs|) (the OLD, globally-
 * centered definition) when fewer than 2 drift-trace windows exist (too
 * little material to have a meaningful local trend) -- documented, not
 * hidden, because it changes what a very short region's sigma means.
 *
 * Bimodality (proposal 40 AC (f)): two residual clusters >= bimodalMinGapMs
 * apart, each carrying >= bimodalMinFrac of the region's events, computed on
 * the (bleed-gated, see twGrooveStatsParams::bleedGateDb) RAW residuals --
 * a stable two-cluster split is a different phenomenon from local drift and
 * is not detrended. When flagged, muMs/sigmaMs are STILL the blended
 * (all-events) statistic -- a caller must read modeAMs/modeBMs instead of
 * muMs when bimodal is true; this struct itself never averages the two into
 * one number that hides the split.
 */
struct twGrooveRegionStats {
    bool   hasData  = false;
    int    nEvents  = 0;   // AFTER the bleed gate (twGrooveStatsParams::bleedGateDb)
    double muMs     = 0.0;   // median residual, ms (meaningless alone if bimodal)
    double sigmaMs  = 0.0;   // 1.4826 * MAD around the LOCAL (drift-trace) median, ms
    bool   bimodal  = false;
    double modeAMs  = 0.0;   // only meaningful when bimodal
    double modeBMs  = 0.0;
    std::vector<twGrooveDriftSample> drift;   // windowed local-mu trace
};

/** One twGrooveRegionStats per region index (matches twGrooveField::region*). */
struct twGrooveResidualReport {
    std::vector<twGrooveRegionStats> perRegion;
};

/**
 * Free parameters both the residual-stat pooling (below) and the pendulum
 * core (twgroovependulum.h) share, so the two estimators are measured with
 * one shared notion of "how wide is a drift window" and "what counts as
 * bimodal". Every field is a recorded, documented default -- proposal 40 M0
 * house rule: no free parameter is hidden inside a magic number.
 */
struct twGrooveStatsParams {
    double driftWindowSec  = 4.0;    // window for the mu-drift trace AND for
                                      // sigma's local-median detrending (same
                                      // windows, see twGrooveRegionStats::sigmaMs)
    double driftStepSec    = 1.0;    // slide step for the drift trace
    double bimodalMinGapMs = 8.0;    // AC (f): "two clusters >= 8 ms apart"
    double bimodalMinFrac  = 0.30;   // AC (f): "each >= 30% of mass"

    // Cross-region bleed gate (coordinator-directed, second M0 iteration).
    // A fast (~0.5 ms attack) transient is broadband by construction, so a
    // genuine event in one region deposits a low-level "bleed" pick in every
    // OTHER region's own event picker too (measured: a0/a_offset15's real
    // fixture WAVs populate all 10 regions at similar counts even though the
    // content is 2-3 distinct tones). Per region, BEFORE mu/sigma/bimodal/
    // drift are computed: the reference level is the median amplitude of the
    // region's own top (loudest) half of events (sorted descending, the
    // "upper mode" -- the level a genuine local voice sits at), and any
    // event more than bleedGateDb below it (amp < refLevel*10^(-bleedGateDb/
    // 20)) is dropped. A region whose events are ALL bleed (no genuine local
    // voice) legitimately ends up with few/no surviving events -- hasData
    // becomes false rather than reporting a wrong mu from noise.
    //
    // MEASURED (second M0 iteration): on the M0 fixture set this gate fires
    // ZERO times -- dumped every region's own amplitude distribution on
    // a0_broadband_grid.wav, a_offset15.wav and f_overlap_streams.wav and
    // found each region's ~60-95 events sit within a percent or two of ONE
    // amplitude (e.g. region 0 of a_offset15.wav: min=median=max=0.08597
    // across all 63 events). The SAME click repeats identically every beat,
    // so there is no loud-primary-vs-quiet-bleed split WITHIN a region for
    // this gate to find; what varies is the level ACROSS regions (region 3
    // of a_offset15.wav sits at 0.0110, region 7 at 0.1172, an 18 dB spread)
    // -- a real effect, but a CROSS-region one this gate cannot see by
    // construction (it only ever compares a region's events against each
    // other). AC (a0)/(a)/(f)'s residual spread survives this gate
    // unchanged for exactly that reason -- see plan/STATE.md or the M0 PR
    // body for the honest accounting rather than re-deriving it here.
    double bleedGateDb = 12.0;
};

/**
 * One front-end event already scored against whichever reference the CALLER
 * uses (the baseline's local window fit, or the pendulum's frozen reference
 * phase) -- residualMs plus the event's own amplitude, which is what the
 * bleed gate (twGrooveStatsParams::bleedGateDb) filters on. Both estimators
 * build one std::vector<twGrooveScoredEvent> per region and hand it to
 * twGroovePoolRegionStats, so the gate and the sigma definition are
 * identical between them BY CONSTRUCTION (one implementation, not two kept
 * in sync by hand).
 */
struct twGrooveScoredEvent {
    double tSec       = 0.0;
    double residualMs = 0.0;
    float  amp        = 0.0f;
};

/**
 * Pools one region's scored events into a twGrooveRegionStats: applies the
 * bleed gate, then computes muMs, the drift trace, the LOCAL-median sigma,
 * and bimodality (twGrooveRegionStats' own doc has the exact definitions).
 * `totalSec` bounds the drift-trace windows (0..totalSec). Shared by BOTH
 * estimators so a caller comparing them is comparing the SAME pooling, not
 * two hand-synchronized copies.
 */
twGrooveResidualReport twGroovePoolRegionStats(
    const std::vector<std::vector<twGrooveScoredEvent>> &eventsByRegion,
    double totalSec, const twGrooveStatsParams &stats );

// =============================================================================
// Baseline estimator -- deliberately simple (M0 section 6 spec): tatum period
// from the merged event train (IOI histogram over near-simultaneous-event
// clusters), a sliding-window robust linear pulse fit (least squares with one
// round of outlier rejection), per-event residual against the LOCAL fit,
// pooled into the same twGrooveRegionStats shape the pendulum core produces.
//
// No grid is assumed a priori: the tatum period and every window's phase are
// RECOVERED from the event train itself, matching proposal 40's "the
// reference is not a quantization grid" refinement even in the dumb estimator.
// =============================================================================

struct twGrooveBaselineParams {
    // Tatum recovery (twGrooveBaselineAnalyze step 1). PRIMARY method:
    // autocorrelation of the summed region flux over lags in
    // [minTatumSec, maxTatumSec] (a "musically plausible" pulse-rate band),
    // peak lag refined by parabolic interpolation -- robust to a noisy/wash-
    // heavy signal generating extra spurious EVENTS, because it never looks
    // at picked events at all. FALLBACK (only when the field is too short to
    // cover the candidate lag range): an IOI histogram over the merged event
    // train, after collapsing near-simultaneous events (within coincidenceMs
    // of each other, e.g. a low+high burst on the same nominal beat) into one
    // cluster time, histogrammed at tatumHistBinMs resolution; the reported
    // tatum is the mean of the IOIs within +-tatumRefineBandMs of the winning
    // bin's center (one refinement pass, not iterative).
    double coincidenceMs     = 15.0;
    double minTatumSec       = 0.1;
    double maxTatumSec       = 2.0;
    double tatumHistBinMs    = 5.0;
    double tatumRefineBandMs = 15.0;

    // Local pulse fit (twGrooveBaselineAnalyze step 2): windows of
    // windowSec length, slid every stepSec (so consecutive windows overlap
    // unless stepSec >= windowSec); a window's fit assigns each event an
    // integer tatum index by rounding (t - windowMeanT)/tatumPeriodSec, fits
    // t = phase0 + tatumPeriodSec_local * index by ordinary least squares,
    // drops any event whose fit residual exceeds outlierRejectMs, and refits
    // once over the survivors. An event's OWN residual is read from whichever
    // window's CENTER is nearest to the event's time (never averaged across
    // overlapping windows -- one fit, one residual, per event).
    double windowSec        = 4.0;
    double stepSec          = 1.0;
    double outlierRejectMs  = 30.0;

    twGrooveStatsParams stats;
};

/** Baseline estimator's output: the recovered tatum plus the pooled stats. */
struct twGrooveBaselineResult {
    double tatumPeriodSec = 0.0;   // 0.0 if fewer than 2 usable IOIs (no lock)
    twGrooveResidualReport residuals;
};

/**
 * Autocorrelation of the summed region flux over an ARBITRARY lag range --
 * the same primitive twGrooveBaselineAnalyze's tatum recovery uses
 * internally, exposed here because the pendulum core (twgroovependulum.h)
 * needs a SECOND instance of it at a slower lag range. Found necessary on
 * fixture (d) (proposal 40 M0 AC (d)): a bar is not reliably a fixed multiple
 * of the recovered TATUM -- d_twobar.wav's own finest regular pulse is the
 * QUARTER note (4 tatums/bar in the usual 8-tatums/bar eighth-note reading),
 * so seeding a "2-bar" unit as tatum*16 misses the true period by 2x, well
 * outside the omega clamp's capture range. Asking THIS function for the
 * period directly, over a lag range wide enough to contain a bar however
 * many tatums it turns out to have, sidesteps the assumption instead of
 * hard-coding a meter.
 *
 * Returns 0.0 if the field is too short to cover [minPeriodSec,maxPeriodSec]
 * or the field/range is otherwise invalid.
 */
double twGrooveRecoverPeriodByAutocorrelation( const twGrooveField &field,
                                               double minPeriodSec, double maxPeriodSec );

/**
 * Runs the baseline estimator over a front-end field (see twgroove.h step
 * documentation for the analyzed field's contents). Returns a
 * default-constructed (tatumPeriodSec == 0, empty residuals) result on
 * field.events.empty() or field.nHops == 0.
 */
twGrooveBaselineResult twGrooveBaselineAnalyze( const twGrooveField &field,
                                                const twGrooveBaselineParams &params );

#endif
