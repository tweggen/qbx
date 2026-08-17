#include "app/shell/sliveinputsource.h"

#include <algorithm>
#include <cmath>

#include "tw/devices/audio_input.h"

SLiveAudioInputSource::SLiveAudioInputSource( audio::AudioInput *input,
                                              unsigned mask, idx_t outChannels,
                                              length_t blockFrames )
    : input_( input )
{
    devChannels_ = input_ ? input_->getConfig().channels : 1u;
    if( devChannels_ < 1u ) devChannels_ = 1u;
    if( outChannels < 1 ) outChannels = 1;
    if( blockFrames < 1 ) blockFrames = 1;

    // Which DEVICE channels the mask selects, in order. An empty selection is
    // channel 0: `audio:default:0` and `audio:default:` both mean "the first
    // channel", which is what an unqualified spelling should do.
    std::vector<int> selected;
    for( unsigned c = 0; c < devChannels_ && c < 32u; ++c )
        if( mask & ( 1u << c ) ) selected.push_back( (int) c );
    if( selected.empty() ) selected.push_back( 0 );

    srcOfOut_.resize( (std::size_t) outChannels );
    for( idx_t c = 0; c < outChannels; ++c ) {
        const std::size_t i = std::min( (std::size_t) c, selected.size() - 1 );
        srcOfOut_[(std::size_t) c] = selected[i];
    }

    scratch_.assign( (std::size_t) blockFrames * devChannels_, 0.0f );
}

std::size_t SLiveAudioInputSource::pull( float *const *out, std::size_t channels,
                                         std::size_t frames, offset_t /*pos*/ )
{
    if( !input_ || !out || frames == 0 ) return 0;

    // Never ask for more than the scratch holds. The plan sized it for the
    // device block, and the pump renders exactly that -- a bigger request
    // would be a plan/pump disagreement, and truncating is the only
    // allocation-free answer.
    const std::size_t cap = scratch_.size() / devChannels_;
    if( frames > cap ) frames = cap;

    const std::int32_t got = input_->read( scratch_.data(), frames );
    if( got <= 0 ) return 0;
    const std::size_t n = (std::size_t) got;

    double peak = peak_.load( std::memory_order_relaxed );
    for( std::size_t c = 0; c < channels; ++c ) {
        float *dst = out[c];
        if( !dst ) continue;
        const std::size_t sc =
            ( c < srcOfOut_.size() ) ? (std::size_t) srcOfOut_[c] : 0u;
        const float *src = scratch_.data() + sc;
        for( std::size_t i = 0; i < n; ++i ) {
            const float v = src[i * devChannels_];
            dst[i] = v;
            const double a = ( v < 0.0f ) ? -(double) v : (double) v;
            if( a > peak ) peak = a;
        }
    }
    peak_.store( peak, std::memory_order_relaxed );
    pulled_.fetch_add( n, std::memory_order_relaxed );
    return n;
}

double SLiveAudioInputSource::takePeak()
{
    return peak_.exchange( 0.0, std::memory_order_relaxed );
}
