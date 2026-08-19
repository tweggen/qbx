
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

        // Proposal 28 W1: optional USER base map — breakpoints in the
        // INTERNAL stretched domain as (out, in) pairs, strictly increasing
        // in both coordinates, origin (0,0) implicit; the tail resumes the
        // base rate S to the end of the input. Empty = uniform stretch S.
        // Transient protection zones are inserted strictly INSIDE base
        // segments (a user anchor is authoritative — protection never moves
        // one), and only where the segment's local slope exceeds 1.
        std::vector<std::pair<double, double>> userMap;   // (out, in)

        // Proposal 28 W4: OPT-IN formant preservation for the pitch stage.
        // Per synthesis frame, the cepstrally-liftered spectral envelope E of
        // the mono-fold magnitudes is estimated and every synthesis bin b is
        // scaled by E(b·pitchRatio)/E(b), so after the sinc resample the
        // output envelope matches the source envelope (formants stay put
        // while the harmonics move). Default OFF — the proposal-26
        // measurement stands: preservation colours general material; this is
        // for vocal/formant-bearing clips. A no-op when pitchRatio == 1, so
        // the OFF path and all pitch-free paths are byte-identical to
        // pre-W4 output.
        bool preserveFormants = false;

        // Independent formant shift (a later, separate feature): a target
        // spectral-envelope ratio applied REGARDLESS of pitchRatio and
        // preserveFormants -- the user's own "formant shift in semitones"
        // knob, not a pitch side-effect. Default 1.0 = no-op (byte-identical
        // to the pre-existing output whatever pitchRatio/preserveFormants are).
        //
        // Composes with preserveFormants by a single combined envelope-warp
        // ratio (derived once in init(), see envRatio): let
        //   baseFormantRatio = preserveFormants ? 1.0 : pitchRatio
        //   T                = baseFormantRatio * formantRatio
        //   envRatio         = pitchRatio / T
        // T is "how far the formants end up moving from their source
        // position, as a ratio" -- 1.0 with preserveFormants and no shift
        // (formants pinned), pitchRatio with neither (formants follow pitch,
        // the historic default), and formantRatio on top of either baseline.
        // wEnv[b] = E(b*envRatio)/E(b) is exactly the W4 formula, generalized
        // to this one ratio, so every existing preserveFormants-only
        // configuration (formantRatio == 1.0) reduces to the identical
        // envRatio == pitchRatio the W4 code always used -- byte-identical.
        // A pure formant shift (preserveFormants == false, pitchRatio == 1.0)
        // gives envRatio == 1/formantRatio, independent of pitch entirely, and
        // runs through the SAME pre-resample envelope step -- it does not
        // require the pitch stage, so it takes effect even when pitchRatio
        // == 1.0 (see twPagedVocoder::Impl::render's identity fast path).
        double formantRatio = 1.0;
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
