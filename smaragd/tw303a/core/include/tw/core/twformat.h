#ifndef _TWFORMAT_H_
#define _TWFORMAT_H_

#include <cstddef>
#include <cstdint>
#include <vector>

// The binary representation of one sample. The engine's native exchange type is
// Float32 (== sample_t); the other encodings exist so a wire can advertise data
// in the form its producer naturally holds (e.g. a WAV-backed source emitting
// Int16) and let the consuming sink decide whether/how to convert.
//
// Values are serialized (project files, debug logs) — append new encodings,
// never renumber existing ones.
enum class twSampleType : std::uint8_t {
    Float32 = 0,   // sample_t — engine canonical, normalized [-1, 1]
    Float64 = 1,
    Int16   = 2,   // e.g. WAV writer, ALSA S16_LE, imported PCM
    Int32   = 3,
};

// How channels are laid out in a multi-channel buffer.
enum class twLayout : std::uint8_t {
    Interleaved = 0,   // L R L R …   (matches the speaker fan-out)
    Planar      = 1,   // L L … R R …
};

// Describes the data flowing through one wire. A default-constructed twFormat is
// byte-for-byte what the engine produces today (mono Float32), so existing code
// that never inspects formats keeps behaving identically.
struct twFormat {
    std::uint32_t sampleRate = 44100;
    twSampleType  sampleType = twSampleType::Float32;
    std::uint16_t channels   = 1;
    twLayout      layout     = twLayout::Interleaved;

    std::size_t bytesPerSample() const
    {
        switch( sampleType ) {
            case twSampleType::Float32: return 4;
            case twSampleType::Float64: return 8;
            case twSampleType::Int16:   return 2;
            case twSampleType::Int32:   return 4;
        }
        return 0;
    }

    std::size_t bytesPerFrame() const { return bytesPerSample() * channels; }

    bool operator==( const twFormat &o ) const
    {
        return sampleRate == o.sampleRate
            && sampleType == o.sampleType
            && channels   == o.channels
            && layout     == o.layout;
    }
    bool operator!=( const twFormat &o ) const { return !( *this == o ); }

    // True when a buffer of *this can be handed to a consumer expecting `other`
    // with a plain memcpy: same type, channel count and layout. Sample rate is
    // metadata (pitch/timing), not memory shape, so it is deliberately ignored
    // here — a rate difference needs a resampler, not a reinterpret.
    bool sameMemoryShape( const twFormat &other ) const
    {
        return sampleType == other.sampleType
            && channels   == other.channels
            && layout     == other.layout;
    }
};

// The engine's canonical exchange format at a given rate: mono Float32. Every
// legacy code path is exactly this with rate == env.getSRate().
inline twFormat twCanonicalFormat( std::uint32_t rate )
{
    return twFormat{ rate, twSampleType::Float32, 1, twLayout::Interleaved };
}

// What a wire *can* carry — the candidate domain for one port, the input to
// format negotiation (proposal 04 §3a), as opposed to twFormat which is the one
// format finally chosen. An empty vector for a dimension means "no constraint"
// (any); the negotiator intersects an empty `rates` with its candidate set D at
// the start of a pass, after which an empty `rates` means *infeasible*.
struct twFormatCaps {
    std::vector<std::uint32_t> rates;          // empty = any
    std::vector<twSampleType>  types;          // empty = any
    std::vector<std::uint16_t> channelCounts;  // empty = any
    std::uint32_t preferredRate = 0;           // 0 = defer to project rate
};

// The candidate domains of all of a node's ports, passed to narrowCaps() so a
// node's coupling relation can narrow inputs and outputs together in one shot.
struct twPortDomains {
    std::vector<twFormatCaps> in;    // indexed by input plug
    std::vector<twFormatCaps> out;   // indexed by output latch
};

#endif
