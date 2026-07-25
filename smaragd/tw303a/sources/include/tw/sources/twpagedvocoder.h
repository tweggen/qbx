
#ifndef _TWPAGEDVOCODER_H_
#define _TWPAGEDVOCODER_H_

#include <cstdint>

/**
 * twPagedVocoder — the in-house phase-vocoder time-stretch / pitch-shift
 * engine (proposal 27 M3). M3 ships the OFFLINE whole-signal mode as an
 * alternate twGrainSource backend behind TW_STRETCH_BACKEND=vocoder; M4 adds
 * keyframed random access (windowed resynthesis from a persisted analysis
 * sidecar); M5 makes it the streaming default.
 *
 * Algorithm (deterministic, single-threaded, double-precision internals):
 *  - Hann STFT analysis at fixed hop Ha over the input.
 *  - Time-stretch by the internal factor S = stretch × pitchRatio using a
 *    fixed SYNTHESIS hop Hs = Ha and a fractional analysis position
 *    a_k = k·Hs/S per synthesis frame k: the nearest analysis frame supplies
 *    the spectrum, phases advance by the instantaneous frequency estimated
 *    from the neighboring analysis-frame phase difference.
 *  - Identity phase-locking (Laroche/Dolson): only spectral-peak phases are
 *    propagated; every other bin keeps its analysis-phase offset relative to
 *    its governing peak — the classic fix for phase-vocoder "phasiness".
 *  - Cross-channel coherence: the peak map and per-bin phase ROTATION are
 *    computed once from a mono fold and applied identically to every
 *    channel's spectrum, so inter-channel phase relationships survive
 *    exactly (a stereo image cannot smear against itself).
 *  - Pitch: the stretched signal (duration ×S) is resampled by pitchRatio
 *    with a 32-tap Kaiser-windowed sinc, landing at duration ×stretch and
 *    pitch ×pitchRatio.
 *  - The caller dictates the EXACT output length (proposal 18 render-
 *    boundary rule: floor(inLen × stretchFrac) computed rationally); the
 *    vocoder fills exactly outLen frames, zero-padding or truncating its
 *    own tail, so all position math upstream is untouched.
 *
 * No Qt, no engine state, no threads, no allocation surprises at read time —
 * this is an offline builder. Runs on worker/UI threads, never RT.
 */
class twPagedVocoder {
public:
    struct Config {
        uint32_t fftSize     = 2048;  // power of two >= 256
        uint32_t analysisHop = 512;   // <= fftSize/2; Hs = this too
        int      rate        = 48000;
        uint32_t channels    = 1;
    };

    /**
     * Offline whole-signal warp. in: planar channel pointers, inLen frames
     * each. out: planar, channels × outLen, caller-allocated (zero-filled by
     * this call). stretch = output/input duration ratio (> 0);
     * pitchRatio = 2^(cents/1200) (> 0). Deterministic: identical inputs and
     * params produce identical bytes on a given platform.
     */
    static void warpOffline( const float *const *in, uint64_t inLen,
                             float *out, uint64_t outLen,
                             const Config &cfg,
                             double stretch, double pitchRatio );
};

#endif
