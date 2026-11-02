#ifndef _TWSAMPLERSTRETCH_H_
#define _TWSAMPLERSTRETCH_H_

// twsamplerstretch.h — the tempo modes' STRETCH method, rendered ahead
// (proposal 56 §4.8, Q4; nassau-digital DESIGN §5, §6.1, §13).
//
// A `tempo-loop` / `tempo-oneshot` zone whose method is `stretch` plays a
// buffer rendered to the song tempo by the host: the layer's source, stretched
// by the SAME phase vocoder qbx uses everywhere (twPagedVocoder, tw/sources),
// pitch kept. The core plays that buffer at its own rate and scales the
// layer's frame positions (start, end, loop points, the velToStart skip) by
// the buffer's twSamplerSample::timeScale(), which is the stretch factor
// (DESIGN §13, settled 2026-11-02: option A). The program keeps SOURCE frames.
//
// Neither function touches Qt, a model object or the audio thread: the app's
// generation job calls them on its worker (objects/sampler). Deterministic:
// the same source, factor and onsets give the same bytes (R8).

#include "tw/sampler/twsamplertypes.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace audio {

// What one layer's stretch is computed from: the song tempo and the layer's
// own attributes, in SOURCE frames exactly as the program holds them (-1 =
// unset, as in the core).
struct twSamplerStretchSpec {
    double       bpm       = 0.0;
    double       beats     = 0.0;
    std::int64_t start     = 0;
    std::int64_t end       = -1;
    std::int64_t loopStart = -1;
    std::int64_t loopEnd   = -1;

    bool operator==( const twSamplerStretchSpec &o ) const
    {
        return bpm == o.bpm && beats == o.beats && start == o.start && end == o.end &&
               loopStart == o.loopStart && loopEnd == o.loopEnd;
    }
    bool operator!=( const twSamplerStretchSpec &o ) const { return !( *this == o ); }
};

// The stretch factor s (output length over source length) that makes the
// layer's `beats` last `beats` beats at `bpm`:
//     s = (beats * 60 / bpm) / (loop frames / rate)
// `beats` spans the loop [loopStart, loopEnd), clamped into [start, end) the
// way the core clamps it; without loop points it spans [start, end) (DESIGN
// §5, §13). 0 when nothing can be stretched: beats <= 0, bpm <= 0, a rate
// <= 0, or a span shorter than 2 frames.
double twSamplerStretchFactor( const twSamplerStretchSpec &spec, std::int64_t frames,
                               double rate );

// The whole source (`channels` 1 or 2, PLANAR, `frames` frames at `rate`)
// stretched by `factor` through twPagedVocoder (fft 2048, hop 512, pitch
// ratio 1: the twGrainSource configuration), with `onsets` (ascending SOURCE
// frames, may be null) as transient keyframes. The result has
// llround(frames * factor) frames, the source's rate, `generation`, and
// timeScale() == factor. Uniform: a lead-in before the loop and a tail after
// it are stretched by the same factor (DESIGN §5: "a lead-in before the loop
// plays at the same rate"). Null with `error` on a bad shape or factor.
// Allocates; never on the audio thread.
std::shared_ptr<const twSamplerSample> twSamplerStretch( int channels, double rate,
                                                         const float *const *planar,
                                                         std::int64_t frames, double factor,
                                                         const std::uint64_t *onsets,
                                                         std::size_t nOnsets,
                                                         std::uint64_t generation,
                                                         std::string *error = nullptr );

}  // namespace audio

#endif
