#include "tw/core/twconvert.h"
#include "tw/core/twlog.h"

#include <atomic>
#include <cstdint>
#include <cstring>

namespace {

// Decode one sample of any supported type to a normalized double in [-1, 1].
// Int reads divide by 2^(bits-1) so full-scale negative maps to exactly -1.0.
inline double readNorm( twSampleType t, const void *base, std::size_t idx )
{
    switch( t ) {
        case twSampleType::Float32: return (double) ( (const float   *) base )[idx];
        case twSampleType::Float64: return         ( (const double  *) base )[idx];
        case twSampleType::Int16:   return (double) ( (const int16_t *) base )[idx] / 32768.0;
        case twSampleType::Int32:   return (double) ( (const int32_t *) base )[idx] / 2147483648.0;
    }
    return 0.0;
}

// Encode a normalized double into one sample of any supported type. Int writes
// scale by the positive full-scale (2^(bits-1)-1) and clamp to the type range,
// matching the prior hand-rolled clip loops in the backends and the WAV writer.
inline void writeNorm( twSampleType t, void *base, std::size_t idx, double v )
{
    switch( t ) {
        case twSampleType::Float32: ( (float  *) base )[idx] = (float) v; break;
        case twSampleType::Float64: ( (double *) base )[idx] = v;         break;
        case twSampleType::Int16: {
            double s = v * 32767.0;
            if( s < -32768.0 )      s = -32768.0;
            else if( s > 32767.0 )  s =  32767.0;
            ( (int16_t *) base )[idx] = (int16_t) s;
            break;
        }
        case twSampleType::Int32: {
            double s = v * 2147483647.0;
            if( s < -2147483648.0 )     s = -2147483648.0;
            else if( s > 2147483647.0 ) s =  2147483647.0;
            ( (int32_t *) base )[idx] = (int32_t) s;
            break;
        }
    }
}

}  // namespace

length_t twConvertFrames( const twFormat &src, const void *srcBuf,
                          const twFormat &dst, void       *dstBuf,
                          length_t frames )
{
    if( frames <= 0 || !srcBuf || !dstBuf ) return 0;

    // PLANAR IS STILL REFUSED, and after proposal 36 that is a decision rather
    // than a gap (§7 trap 6, reviewed at B9). The engine IS planar now — a
    // twOutputPage is `channels` planes at a constant stride — but that shape
    // never crosses this function: every twFormat in the tree keeps the default
    // twLayout::Interleaved, twLayout::Planar is assigned nowhere, and this
    // function's three callers (the WAV writer, the WASAPI backend, the ALSA
    // backend) are all device/file wires that are interleaved by definition. So
    // there is no wire that needs planar support, and writing some on
    // speculation would be untested conversion code on the audio path.
    //
    // What DID need fixing is that the refusal was silent. Returning 0 with no
    // word is indistinguishable from "nothing to do", so the day somebody does
    // set a planar layout here the symptom would be silence at the device with
    // nothing in the log. It is a one-shot report because this is per-buffer
    // code on the callback path.
    if( src.layout != twLayout::Interleaved || dst.layout != twLayout::Interleaved ) {
        static std::atomic<bool> reported{ false };
        if( !reported.exchange( true ) ) {
            TW_LOGW( "core", "[twConvertFrames] planar layout is not supported "
                             "(src=%d dst=%d); converting nothing. The engine's "
                             "planar shape is twOutputPage, which never reaches "
                             "this wire -- see proposal 36 trap 6.",
                     (int) src.layout, (int) dst.layout );
        }
        return 0;
    }

    // Same type, channels and layout → a straight copy (e.g. Float32->Float32).
    if( src.sameMemoryShape( dst ) ) {
        std::memcpy( dstBuf, srcBuf, (std::size_t) frames * src.bytesPerFrame() );
        return frames;
    }

    const std::uint16_t sc = src.channels ? src.channels : 1;
    const std::uint16_t dc = dst.channels ? dst.channels : 1;

    for( length_t f = 0; f < frames; ++f ) {
        for( std::uint16_t c = 0; c < dc; ++c ) {
            double v;
            if( sc == dc ) {
                v = readNorm( src.sampleType, srcBuf, (std::size_t)( f * sc + c ) );
            } else if( sc == 1 ) {
                v = readNorm( src.sampleType, srcBuf, (std::size_t) f );          // mono → N
            } else if( dc == 1 ) {
                double acc = 0.0;                                                 // N → mono
                for( std::uint16_t s = 0; s < sc; ++s )
                    acc += readNorm( src.sampleType, srcBuf, (std::size_t)( f * sc + s ) );
                v = acc / (double) sc;
            } else {
                // Mismatched multi→multi: map shared channels, silence the rest.
                v = ( c < sc )
                  ? readNorm( src.sampleType, srcBuf, (std::size_t)( f * sc + c ) )
                  : 0.0;
            }
            writeNorm( dst.sampleType, dstBuf, (std::size_t)( f * dc + c ), v );
        }
    }
    return frames;
}
