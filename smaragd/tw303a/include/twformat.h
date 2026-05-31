#ifndef _TWFORMAT_H_
#define _TWFORMAT_H_

#include <cstddef>
#include <cstdint>

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

#endif
