// twSamplerStretch: the stretch method's renderer (proposal 56 §4.8, Q4).

#include "tw/sampler/twsamplerstretch.h"

#include "tw/sources/twpagedvocoder.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace audio {

double twSamplerStretchFactor( const twSamplerStretchSpec &spec, std::int64_t frames, double rate )
{
    if( !( spec.beats > 0.0 ) || !( spec.bpm > 0.0 ) || !( rate > 0.0 ) || frames < 2 ) return 0.0;
    // The core's own clamp (nd_instrument.cpp, note-on), so the span stretched
    // to the tempo is exactly the span the core loops.
    auto clampI = []( std::int64_t v, std::int64_t lo, std::int64_t hi ) {
        return v < lo ? lo : ( v > hi ? hi : v );
    };
    const std::int64_t hi     = spec.end < 0 ? frames : std::min( spec.end, frames );
    const std::int64_t lo     = std::min( std::max<std::int64_t>( spec.start, 0 ), hi );
    const std::int64_t loopLo = spec.loopStart < 0 ? lo : clampI( spec.loopStart, lo, hi );
    const std::int64_t loopHi = spec.loopEnd < 0 ? hi : clampI( spec.loopEnd, loopLo, hi );
    const std::int64_t span   = loopHi - loopLo;
    if( span < 2 ) return 0.0;
    const double s = spec.beats * 60.0 * rate / ( spec.bpm * static_cast<double>( span ) );
    return ( s > 0.0 && std::isfinite( s ) ) ? s : 0.0;
}

std::shared_ptr<const twSamplerSample> twSamplerStretch( int channels, double rate,
                                                         const float *const *planar,
                                                         std::int64_t frames, double factor,
                                                         const std::uint64_t *onsets,
                                                         std::size_t nOnsets,
                                                         std::uint64_t generation,
                                                         std::string *error )
{
    if( channels != 1 && channels != 2 ) {
        if( error ) *error = "a sample has 1 or 2 channels";
        return nullptr;
    }
    if( !( factor > 0.0 ) || !std::isfinite( factor ) ) {
        if( error ) *error = "the stretch factor is not finite and positive";
        return nullptr;
    }
    if( frames <= 0 || !planar || !( rate > 0.0 ) ) {
        if( error ) *error = "nothing to stretch";
        return nullptr;
    }
    const double outD = std::llround( static_cast<double>( frames ) * factor );
    if( outD < 1.0 || outD > 1.0e10 ) {
        if( error ) *error = "the stretched length is out of range";
        return nullptr;
    }
    const std::uint64_t outLen = static_cast<std::uint64_t>( outD );

    twPagedVocoder::Config cfg;
    cfg.fftSize     = 2048;
    cfg.analysisHop = 512;
    cfg.rate        = static_cast<int>( std::lround( rate ) );
    cfg.channels    = static_cast<std::uint32_t>( channels );
    std::vector<float> out( static_cast<std::size_t>( outLen ) * static_cast<std::size_t>( channels ),
                            0.0f );
    twPagedVocoder::warpOffline( planar, static_cast<std::uint64_t>( frames ), out.data(), outLen,
                                 cfg, factor, 1.0, nOnsets ? onsets : nullptr, nOnsets );
    const float *chans[2] = { out.data(),
                              channels > 1 ? out.data() + static_cast<std::size_t>( outLen ) : nullptr };
    return twSamplerSample::fromPlanar( channels, rate, chans, static_cast<std::int64_t>( outLen ),
                                        generation, error, factor );
}

}  // namespace audio
