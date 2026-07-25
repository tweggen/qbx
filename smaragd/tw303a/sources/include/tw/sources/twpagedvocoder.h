
#ifndef _TWPAGEDVOCODER_H_
#define _TWPAGEDVOCODER_H_

#include <cstdint>
#include <memory>
#include <vector>

/**
 * twPagedVocoder — the in-house phase-vocoder time-stretch / pitch-shift
 * engine (proposal 27, M3 core + M4 random access).
 *
 * Algorithm (deterministic, single-threaded, double-precision math, float
 * analysis storage):
 *  - Lazy Hann-STFT analysis per frame over the borrowed source PCM
 *    (magnitude + phase per channel and for the mono fold; computed on
 *    demand, cached in memory). Analysis is INCREMENTAL by construction —
 *    this engine needs no persisted analysis sidecar: a per-frame windowed
 *    FFT over resident samples is cheaper than reading persisted spectra.
 *  - Time-stretch by S = stretch × pitchRatio: fixed synthesis hop Hs = Ha,
 *    fractional analysis position per synthesis frame, IF-based phase
 *    propagation.
 *  - Identity phase-locking (Laroche/Dolson) with PROMINENCE GATING (M4):
 *    bins are locked to their governing spectral peak only when that peak
 *    stands out from the frame's median magnitude by `peakProminence`;
 *    bins under non-prominent (spurious, noise-floor) peaks propagate
 *    FREE-RUNNING, so stochastic material keeps its phase incoherence —
 *    the fix for the comb/metallic coloration reported on noisy sounds.
 *  - Cross-channel coherence: one rotation field from the mono fold applied
 *    to every channel.
 *  - KEYFRAMES (M4): at synthesis frame k with k ≡ 0 (mod keyframeInterval)
 *    and at frames aligned with source ONSETS (the M1 "onsets" sidecar
 *    aspect — transient preservation: a reset at an attack re-anchors phase
 *    to the analysis truth), the synthesis phase state RESETS to the
 *    analysis phases. Consequences: (a) random access with bounded
 *    pre-roll — any output window resynthesizes identically by rolling
 *    forward from the nearest keyframe; (b) paged output is BIT-IDENTICAL
 *    to whole-signal output for any partition of the output range (the M4
 *    property gate); (c) transient smear drops because attacks re-anchor.
 *  - Pitch: Kaiser-windowed sinc resample (LUT-accelerated) of the
 *    stretched signal; final output frame i depends only on a bounded
 *    stretched-domain neighborhood, preserving the bit-exactness property
 *    through the pitch stage.
 *
 * The caller dictates the exact output length (proposal 18: floor(inLen ×
 * stretchFrac) computed rationally) and pre-zeroes the destination; the
 * vocoder writes what the material backs, leaving the tail zero.
 *
 * Lifetime: the instance BORROWS the source channel pointers — they must
 * outlive it. No Qt, no threads, no engine state; never on the RT thread.
 */
class twPagedVocoder {
public:
    struct Config {
        uint32_t fftSize          = 2048;  // power of two >= 256
        uint32_t analysisHop      = 512;   // <= fftSize/2; synthesis hop too
        int      rate             = 48000;
        uint32_t channels         = 1;
        uint32_t keyframeInterval = 64;    // synthesis frames per fixed keyframe
        double   peakProminence   = 4.0;   // peak vs frame-median magnitude (~12 dB)
    };

    twPagedVocoder( const float *const *src, uint64_t inLen,
                    const Config &cfg, double stretch, double pitchRatio,
                    const uint64_t *onsets = nullptr, size_t nOnsets = 0 );
    ~twPagedVocoder();

    /**
     * Render final-output frames [outStart, outStart + len) into dst —
     * planar, channel c at dst[c*len .. c*len+len). dst must be zeroed by
     * the caller. Bit-exact under partition: rendering a range in any set
     * of consecutive windows produces byte-identical samples to rendering
     * it in one call.
     */
    void render( uint64_t outStart, uint64_t len, float *dst );

    /** Whole-signal convenience (the twGrainSource offline path). */
    static void warpOffline( const float *const *in, uint64_t inLen,
                             float *out, uint64_t outLen,
                             const Config &cfg,
                             double stretch, double pitchRatio,
                             const uint64_t *onsets = nullptr,
                             size_t nOnsets = 0 );

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif
