#ifndef TW_CORE_POSITION_CODE_H
#define TW_CORE_POSITION_CODE_H

// The "integer-cycle tone staircase" position encoding.
//
// A fixture whose CONTENT names its own ABSOLUTE SOURCE FRAME. The qxa suite is
// structurally render-only and every audio assertion it owns (RMS, peak, f0) is
// position-BLIND: a render that plays the right material from the wrong place
// passes them all. This encoding closes that hole — decode a window of audio,
// learn which source frame it came from.
//
// The encoding
// ------------
// Audio is cut into blocks of kBlockFrames (4096) frames. Block k is a pure
// sine carrying EXACTLY c_k INTEGER cycles across the block, amplitude
// kAmplitude, starting at phase 0:
//
//     x(t) = A * sin(2*pi * c_k * (t - k*B) / B),   t in [k*B, (k+1)*B)
//
// Two properties do all the work:
//
//  1. c_k cycles in B samples means the tone sits EXACTLY on DFT bin c_k of a
//     B-length window: zero spectral leakage. A Goertzel evaluated at the
//     candidate bins is ~0 at every bin but the right one, so the decode is an
//     argmax with an enormous margin — it survives float32 engine arithmetic
//     and a 16-bit PCM render roundtrip untouched.
//  2. Because the tone frequency IS a bin frequency, ANY window of length B
//     that lies inside one block contains exactly c_k cycles regardless of its
//     phase offset. The decoder therefore does not need block-aligned windows,
//     only windows that do not STRADDLE a block boundary (a straddling window
//     lights two bins and is reported as low confidence, never as a wrong
//     answer).
//
// Each block starts and ends at phase 0 (sin(2*pi*c) == 0 with positive slope),
// so the waveform is continuous across block boundaries — no click, and nothing
// for a resampler or a codec to smear into the next block.
//
// The c_k sequence
// ----------------
//     c_0   = 16
//     c_k+1 = max(c_k + 3, round(c_k * 1.03))     while c_k <  kMaxGeomCycles
//     c_k+1 = c_k + 3                             while c_k >= kMaxGeomCycles
//
// The +3 floor keeps neighbouring blocks at least three bins apart at the low
// end; the 3% term keeps them proportionally apart once the bins are large.
//
// THE CEILING IS LOAD-BEARING. Unclamped, 3%-per-block growth reaches bin 10639
// by block 186 — five times the Nyquist bin, i.e. an unrepresentable
// "frequency" and a silently aliased fixture. Disabling the multiplicative term
// at kMaxGeomCycles (= B/4, one half of Nyquist) degrades growth to +3 and
// keeps c_186 = 1265 (14824.22 Hz at 48 kHz), comfortably below bin B/2 = 2048
// and below the corner of any resampler the file might meet. The sequence stays
// strictly increasing, hence still unique per block, for another ~260 blocks
// past the default.
//
// Where this lives, and why
// -------------------------
// tw/core, not tw/analysis: THE GENERATOR AND EVERY DECODER MUST AGREE, and the
// consumers sit in modules that may not include each other. The generator tool
// is in tw/analysis, the file decoder is in tw/analysis, the in-memory decoder
// is in tw/playback's test, and the assert verb is in app/testkit. core is the
// only module all four are allowed to include (check_layering.py), so one
// implementation of the sequence is only possible here.
//
// Integer arithmetic throughout: the sequence must be bit-identical between the
// tool that WROTE the committed fixture and the decoder that reads it, on every
// platform and optimisation level. round(c * 1.03) is spelled (c*103 + 50)/100.

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace tw {
namespace poscode {

// Frames per encoded block.
constexpr int64_t kBlockFrames = 4096;

// Cycle count of block 0.
constexpr int kFirstCycles = 16;

// Minimum bin distance between consecutive blocks.
constexpr int kMinStep = 3;

// Multiplicative growth, as a percentage (103 == 1.03x).
constexpr int kGrowthPercent = 103;

// Above this cycle count the multiplicative term is disabled (see above).
// B/4 == one half of Nyquist.
constexpr int kMaxGeomCycles = (int)(kBlockFrames / 4);

// Block count of the committed fixture (765952 frames, 15.96 s at 48 kHz).
constexpr int kDefaultBlocks = 187;

// Amplitude of the encoded tone.
constexpr double kAmplitude = 0.5;

/// The next cycle count after `c`.
constexpr int nextCycles(int c)
{
    if (c >= kMaxGeomCycles) {
        return c + kMinStep;
    }
    const int geometric = (c * kGrowthPercent + 50) / 100;
    const int additive  = c + kMinStep;
    return geometric > additive ? geometric : additive;
}

/// Cycle count (== DFT bin of a kBlockFrames window) of block `k`.
constexpr int cyclesForBlock(int64_t k)
{
    int c = kFirstCycles;
    for (int64_t i = 0; i < k; ++i) {
        c = nextCycles(c);
    }
    return c;
}

/// First frame of block `k`.
constexpr int64_t frameForBlock(int64_t k) { return k * kBlockFrames; }

/// Block containing absolute frame `f` (negative frames map to block -1 etc.).
constexpr int64_t blockForFrame(int64_t f)
{
    return f >= 0 ? f / kBlockFrames
                  : -(((-f) + kBlockFrames - 1) / kBlockFrames);
}

/// Frequency in Hz of block `k` at `sampleRate`.
inline double frequencyForBlock(int64_t k, int sampleRate)
{
    return (double)cyclesForBlock(k) * (double)sampleRate / (double)kBlockFrames;
}

/// Highest bin the sequence reaches over `blocks` blocks. A caller that raises
/// the block count should check this against kBlockFrames/2.
constexpr int maxCyclesForBlocks(int64_t blocks)
{
    return blocks > 0 ? cyclesForBlock(blocks - 1) : kFirstCycles;
}

// --- Synthesis -------------------------------------------------------------
// The generator tool and the in-memory test component both encode through
// these, so a fixture on disk and a component rendering "the same" signal are
// the same signal by construction.

/// One sample of a block with `cycles` cycles, `t` frames into that block.
inline double sampleInBlock(int cycles, int64_t t)
{
    constexpr double kTwoPi = 6.283185307179586476925286766559;
    return kAmplitude * std::sin(kTwoPi * (double)cycles * (double)t
                                 / (double)kBlockFrames);
}

/// One sample at an ABSOLUTE frame. Convenience for callers that render
/// arbitrary ranges; O(block index), so a bulk generator should hoist
/// cyclesForBlock() out of its inner loop instead.
inline double sampleAtFrame(int64_t absFrame)
{
    if (absFrame < 0) return 0.0;
    const int64_t k = blockForFrame(absFrame);
    return sampleInBlock(cyclesForBlock(k), absFrame - frameForBlock(k));
}

// --- Decoding --------------------------------------------------------------

/// What a window of encoded audio says about where it came from.
struct Decode {
    int64_t blockIndex  = -1;    ///< decoded block, -1 if none
    int64_t sourceFrame = -1;    ///< first frame of that block, -1 if none
    double  confidence  = 0.0;   ///< best bin magnitude / runner-up's
    double  rms         = 0.0;   ///< window RMS (diagnostics)
    bool    silent      = true;  ///< below the silence floor; no decode attempted
};

/// Magnitude of the component at normalized angular frequency `omega`, by
/// Goertzel. Not autocorrelation: a pure sine's autocorrelation is
/// octave-ambiguous (lag 2T correlates as well as lag T), which would decode a
/// block as its own half-frequency neighbour whenever one exists in the
/// candidate set. Goertzel asks a direct question per candidate bin, and on
/// this fixture the wrong bins are exactly orthogonal to the signal.
inline double goertzelMagnitude(const double *x, int64_t n, double omega)
{
    const double c = std::cos(omega);
    const double coeff = 2.0 * c;
    double s1 = 0.0, s2 = 0.0;
    for (int64_t i = 0; i < n; ++i) {
        const double s0 = x[(std::size_t)i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double re = s1 - s2 * c;
    const double im = s2 * std::sin(omega);
    return std::sqrt(re * re + im * im) * 2.0 / (double)n;
}

/// Decode a window of encoded audio to the block it came from.
///
/// Rate-independent: the tone of block k sits at normalized frequency
/// 2*pi*c_k/kBlockFrames whatever the sample rate is, so nothing here needs to
/// know the rate — only that the audio has not been RESAMPLED since it was
/// generated.
///
/// A window that straddles a block boundary lights two candidate bins in
/// proportion to how much of each block it saw, so `confidence` (best over
/// runner-up) falls towards 1. That is the whole point of reporting a
/// confidence: the caller learns "I cannot tell" instead of being handed one of
/// the two arbitrarily.
inline Decode decodeBuffer(const double *x, int64_t n,
                           int64_t candidateBlocks = kDefaultBlocks,
                           double silenceRms = 0.02)
{
    constexpr double kTwoPi = 6.283185307179586476925286766559;
    Decode d;
    if (!x || n <= 0) return d;

    double sumSq = 0.0;
    for (int64_t i = 0; i < n; ++i) sumSq += x[(std::size_t)i] * x[(std::size_t)i];
    d.rms = std::sqrt(sumSq / (double)n);
    if (d.rms < silenceRms) return d;  // silent: no decode attempted
    d.silent = false;

    double best = -1.0, second = -1.0;
    int64_t bestBlock = -1;
    int c = kFirstCycles;
    for (int64_t k = 0; k < candidateBlocks; ++k) {
        const double mag = goertzelMagnitude(x, n, kTwoPi * (double)c
                                                   / (double)kBlockFrames);
        if (mag > best) { second = best; best = mag; bestBlock = k; }
        else if (mag > second) { second = mag; }
        c = nextCycles(c);
    }

    d.blockIndex  = bestBlock;
    d.sourceFrame = bestBlock >= 0 ? frameForBlock(bestBlock) : -1;
    // A runner-up of exactly zero (possible only in synthetic double-precision
    // audio) is reported as a large finite confidence rather than infinity, so
    // the number survives printing and comparison unremarkably.
    d.confidence = (second > 0.0) ? best / second : 1.0e9;
    return d;
}

}  // namespace poscode
}  // namespace tw

#endif  // TW_CORE_POSITION_CODE_H
