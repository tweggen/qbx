
#ifndef _TWGROOVE_H_
#define _TWGROOVE_H_

#include <cstdint>
#include <vector>

/**
 * Groove analysis front end (proposal 40 "Feel Flow", M0). Pure, deterministic,
 * single-threaded function over resident PCM -- no engine state, no I/O, no Qt,
 * no threads, no thread_local, no rand(). Mirrors the house convention of
 * twanalyzers.h: identical input and params produce identical output on a
 * given platform (fixed evaluation order).
 *
 * This is ONLY the excitation front end of proposal 40 section 3.1 -- the
 * cochlea-shaped IIR bank, its group-delay calibration, region pooling and a
 * baseline event picker over the pooled flux. It does NOT implement the
 * pendulum ensemble (section 3.2) or the resonance/counter-tension readouts
 * (section 3.3-3.5); those consume this front end's twGrooveField as their
 * excitation input in a later milestone.
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

#endif
