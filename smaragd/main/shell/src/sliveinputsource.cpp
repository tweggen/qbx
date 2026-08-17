#include "app/shell/sliveinputsource.h"

#include <algorithm>
#include <cmath>

#include "tw/record/capture_bridge.h"

SLiveAudioInputSource::SLiveAudioInputSource( audio::CaptureBridge *bridge,
                                              unsigned mask, idx_t outChannels,
                                              length_t blockFrames )
    : bridge_( bridge )
{
    devChannels_ = bridge_ ? bridge_->inputChannels() : 1u;
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

    // PLANAR since L3b: pullLive() writes one buffer per device channel, which
    // is also what the pump wants, so the interleaved hop is gone. Sized here,
    // on the main thread — pull() allocates nothing.
    scratch_.assign( (std::size_t) blockFrames * devChannels_, 0.0f );
    planes_.resize( devChannels_ );
    for( unsigned c = 0; c < devChannels_; ++c )
        planes_[c] = scratch_.data() + (std::size_t) c * (std::size_t) blockFrames;
}

std::size_t SLiveAudioInputSource::pull( float *const *out, std::size_t channels,
                                         std::size_t frames, offset_t /*pos*/ )
{
    if( !bridge_ || !out || frames == 0 ) return 0;

    // Never ask for more than the scratch holds. The plan sized it for the
    // device block, and the pump renders exactly that -- a bigger request
    // would be a plan/pump disagreement, and truncating is the only
    // allocation-free answer.
    const std::size_t cap = scratch_.size() / devChannels_;
    if( frames > cap ) frames = cap;

    const std::size_t n = bridge_->pullLive( planes_.data(), devChannels_, frames );
    if( n == 0 ) return 0;

    double peak = peak_.load( std::memory_order_relaxed );
    for( std::size_t c = 0; c < channels; ++c ) {
        float *dst = out[c];
        if( !dst ) continue;
        const std::size_t sc =
            ( c < srcOfOut_.size() ) ? (std::size_t) srcOfOut_[c] : 0u;
        const float *src = planes_[ sc < devChannels_ ? sc : 0u ];
        for( std::size_t i = 0; i < n; ++i ) {
            const float v = src[i];
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
