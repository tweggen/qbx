#ifndef _TWSAMPLERTYPES_H_
#define _TWSAMPLERTYPES_H_

// twsamplertypes.h — the two immutable snapshots the sampler instrument is
// handed (proposal 56 §4.2; nassau-digital DESIGN §2, §6, §9.1).
//
// Both are OPAQUE on purpose. The NassauDigital core is linked PRIVATE into
// tw_sampler (proposal 56 invariant 1): no other module, engine or app, may
// see a core header, so nothing in this file may name one. What the core
// holds lives behind Impl, which only tw/sampler's own sources can complete
// (sampler/src/twsamplerimpl.h).
//
// Neither type is ever changed after it is made. New audio is a new sample
// with a higher generation; an edited program is a new program. A snapshot is
// handed to the instrument by shared_ptr through twSamplerHost, and the
// instrument keeps what a sounding voice still reads (DESIGN §6.1, rule 4).
//
// Both factories allocate and parse: call them off the audio thread (a worker
// or the main thread), never from process(). Qt-free (THREADING.md rule 1).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace audio {

// One NassauDigital program: zones, layers and program-level settings, in the
// core's own model. qbx never reads a program attribute itself (proposal 56
// invariant 8): it hands the core the program's XML subtree as text and gets
// text back.
class twSamplerProgram {
public:
    // Parse `xml` — the <NassauDigitalProgram> element as text, exactly what
    // the core's own reader takes. Null on a refusal, with the core's reason
    // in `error`.
    static std::shared_ptr<const twSamplerProgram> fromXml( const std::string &xml,
                                                            std::string *error = nullptr );

    // The program written back by the core (no XML declaration).
    std::string toXml() const;

    // A number unique to this snapshot within the process, never reused: what
    // the instrument reports as the program its process() last ran on
    // (twSamplerHostStats), so a test can tell two installs apart.
    std::uint64_t serial() const { return serial_; }

    std::size_t zoneCount() const;

    ~twSamplerProgram();
    twSamplerProgram( const twSamplerProgram & )            = delete;
    twSamplerProgram &operator=( const twSamplerProgram & ) = delete;

    struct Impl;                       // completed in sampler/src only
    const Impl &impl() const { return *impl_; }

private:
    // twSamplerProgramModel::snapshot() (Q1) makes snapshots from the editable
    // program without a round trip through XML.
    friend class twSamplerProgramModel;

    twSamplerProgram();
    std::unique_ptr<Impl> impl_;
    std::uint64_t         serial_ = 0;
};

// One immutable PCM buffer (DESIGN §2): 1 or 2 channels, its own sample rate,
// its frames, and its GENERATION.
class twSamplerSample {
public:
    // Copy `frames` frames of `channels` PLANAR channels (1 or 2). Null for a
    // shape the core refuses (another channel count, a rate <= 0, negative
    // frames), with the reason in `error`.
    static std::shared_ptr<const twSamplerSample> fromPlanar( int channels, double sampleRate,
                                                              const float *const *planar,
                                                              std::int64_t frames,
                                                              std::uint64_t generation,
                                                              std::string *error = nullptr );

    int           channels() const;
    double        sampleRate() const;
    std::int64_t  frames() const;
    std::uint64_t generation() const;

    ~twSamplerSample();
    twSamplerSample( const twSamplerSample & )            = delete;
    twSamplerSample &operator=( const twSamplerSample & ) = delete;

    struct Impl;                       // completed in sampler/src only
    const Impl &impl() const { return *impl_; }

private:
    twSamplerSample();
    std::unique_ptr<Impl> impl_;
};

}  // namespace audio

#endif
