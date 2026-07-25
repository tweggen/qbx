
#ifndef _TWANALYZERS_H_
#define _TWANALYZERS_H_

#include <cstdint>
#include <vector>

/**
 * Offline per-time analyzers (proposal 27 M1) — pure functions over resident
 * PCM buffers. No engine state, no I/O, no Qt; deterministic: identical input
 * and params produce identical output on a given platform (fixed evaluation
 * order, single-threaded). Runs on background worker threads; never on the RT
 * audio thread. The caller (the analysis job) feeds SOURCE-rate planar
 * channels and serializes the results into QAF sidecars per twaspects.h.
 *
 * Both analyzers fold multi-channel input before analysis:
 *  - onsets: arithmetic mean across channels per frame.
 *  - loudness: power mean (mean of squares across channels and window).
 */

/**
 * Spectral-flux onset detection. Normative algorithm (aspect "onsets" v1 —
 * changing any step bumps twAspect::OnsetsVersion):
 *
 *  1. mono[i] = mean over channels of chans[c][i].
 *  2. STFT frames at t = k*hop for every k with k*hop < nFrames; the window
 *     [t, t+fftSize) is zero-padded past the end. Hann window
 *     w[j] = 0.5 - 0.5*cos(2*pi*j/(fftSize-1)), magnitudes for bins
 *     b in [0, fftSize/2].
 *  3. flux[k] = sum_b max(0, mag_k[b] - mag_{k-1}[b]) / sum_b mag_k[b]
 *     (NORMALIZED positive flux — relative spectral change; v2. Steady
 *     material reads ~0 at any level; a true attack reads O(1)); flux[0]=0,
 *     and a frame with total magnitude <= 1e-12 reads 0.
 *  4. thr[k] = thresholdFactor * median(flux over [k-W, k+W], window clamped
 *     at the edges) + thresholdFloor, W = medianHalfWidth. thresholdFloor is
 *     in NORMALIZED flux units since v2.
 *  5. Candidates: flux[k] > thr[k] AND the frame's total magnitude is at
 *     least 1% of the file's PEAK frame magnitude (the v2 energy gate —
 *     relative flux explodes on quiet crescendos, which are not onsets)
 *     AND flux[k] >= flux[k-1] AND flux[k] > flux[k+1] (first/last frame
 *     are never candidates).
 *  6. Ascending scan: drop any candidate closer than minSeparationFrames to
 *     the last KEPT onset. Position = k*hop + (fftSize - hop/2), clamped to
 *     nFrames-1 (source frames; v4 attack-centering — the flux frame START
 *     leads the perceptual attack by ~fftSize - hop/2 because the transient
 *     enters the analysis window from its tail).
 *
 * fftSize must be a power of two (>= 64); hop must be > 0 and <= fftSize.
 * Violations return an empty result.
 */
struct twOnsetParams {
    uint32_t fftSize             = 1024;
    uint32_t hop                 = 256;
    float    thresholdFactor     = 1.5f;
    float    thresholdFloor      = 0.1f;    // normalized-flux units (v2): a true
    // attack is an O(1) relative spectral change; hop-vs-period beating on
    // steady or swelling tonal material stays in the few-percent range.
    uint32_t medianHalfWidth     = 8;
    uint32_t minSeparationFrames = 0;   // caller: ~30 ms at the source rate

    // Canonical LE params blob for QAF keying (twaspects.h field order).
    void serialize( std::vector<uint8_t> &out ) const;
};

/**
 * One detected onset (aspect "onsets" v3): position in SOURCE frames plus a
 * SALIENCE — the normalized-flux value that triggered the detection. Two
 * consumer tiers (proposal 28 W0): the vocoder's keyframes take every
 * entry (dense-but-cheap, M5 behavior); the marker UI shows only entries
 * whose salience clears its display threshold.
 */
struct twOnset {
    uint64_t pos      = 0;
    float    salience = 0.0f;

    bool operator==( const twOnset &o ) const {
        return pos == o.pos && salience == o.salience;
    }
};

std::vector<twOnset> twDetectOnsets( const float *const *chans, uint32_t nCh,
                                     uint64_t nFrames,
                                     const twOnsetParams &params );

/**
 * RMS envelope (aspect "loudness" v1). rms[k] over the window starting at
 * k*hopFrames, length winFrames, zero-padded past the end — the divisor is
 * ALWAYS winFrames*nCh, so tail records taper rather than jump:
 *   rms[k] = sqrt( sum_{c,j} x[c][k*hop+j]^2 / (winFrames*nCh) ).
 * Result size = ceil(nFrames / hopFrames). hopFrames and winFrames must be
 * > 0; violations return an empty result.
 */
struct twLoudnessParams {
    uint32_t hopFrames = 0;   // caller: sourceRate/100 (10 ms)
    uint32_t winFrames = 0;   // caller: 2*hopFrames

    void serialize( std::vector<uint8_t> &out ) const;
};

std::vector<float> twComputeLoudness( const float *const *chans, uint32_t nCh,
                                      uint64_t nFrames,
                                      const twLoudnessParams &params );

/**
 * Per-hop fundamental estimate (aspect "f0" v1, proposal 28 W3). Normative
 * algorithm — changing any step bumps twAspect::F0Version:
 *
 *  1. mono[i] = mean over channels of chans[c][i] (as the other analyzers).
 *  2. For each hop k at t = k*hopFrames, the YIN difference function over
 *     the integration window W = winFrames:
 *       d(tau) = sum_{j=0}^{W-1} (mono[t+j] - mono[t+j+tau])^2,
 *     tau in [1, tauMax], tauMax = floor(rate/fminHz), samples past nFrames
 *     read as 0 (zero-pad).
 *  3. Cumulative-mean-normalized difference:
 *       d'(1..tau) = d(tau) * tau / sum_{j=1}^{tau} d(j),  d'(0) = 1.
 *  4. Pick the FIRST tau in [tauMin, tauMax], tauMin = ceil(rate/fmaxHz),
 *     with d'(tau) < threshold, then descend while d'(tau+1) < d'(tau)
 *     (local minimum). If none dips below threshold: UNVOICED.
 *  5. Parabolic interpolation of d' around the picked tau (clamped to
 *     +/-1); f0 = rate / tauRefined.
 *  6. Energy gate: a window whose RMS is below 1e-4 is UNVOICED (silence
 *     must not produce spurious pitches).
 *
 * Result: one float32 per hop, size = ceil(nFrames / hopFrames);
 * 0 = unvoiced. All accumulation in double; reductions run in FOUR fixed
 * lanes combined (l0+l2)+(l1+l3) (aspect v2 — the normative order, chosen
 * so scalar and SIMD builds agree bit-exactly on every platform).
 * hopFrames, winFrames must be > 0 and fminHz < fmaxHz (both > 0), with
 * rate > 0; violations return an empty result.
 */
struct twF0Params {
    uint32_t rate      = 0;      // source rate the estimate runs at
    uint32_t hopFrames = 0;      // caller: sourceRate/100 (10 ms)
    uint32_t winFrames = 0;      // caller: sourceRate/30 (~33 ms, >= 2/fmin)
    float    fminHz    = 60.0f;
    float    fmaxHz    = 1000.0f;
    float    threshold = 0.15f;  // CMND absolute threshold (YIN classic)

    // Canonical LE params blob for QAF keying (twaspects.h field order).
    void serialize( std::vector<uint8_t> &out ) const;
};

std::vector<float> twComputeF0( const float *const *chans, uint32_t nCh,
                                uint64_t nFrames, const twF0Params &params );

#endif
