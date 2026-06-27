
#include <math.h>
#include <string.h>

#include "twresampledsource.h"

twResampledSource::twResampledSource( const twRandomSource &src, int targetRate )
    : targetRate_( targetRate ),
      channels_( src.channels() ),
      nFrames_( 0 )
{
    int      srcRate   = src.sampleRate();
    length_t srcFrames = src.length();
    if( srcRate <= 0 || targetRate_ <= 0 || srcFrames <= 0 || channels_ <= 0 ) {
        return;   // nothing to build; read() will zero-fill
    }

    double ratio = (double) targetRate_ / (double) srcRate;
    nFrames_ = (length_t) llround( (double) srcFrames * ratio );
    if( nFrames_ <= 0 ) {
        nFrames_ = 0;
        return;
    }

    data_.assign( (size_t) channels_ * nFrames_, 0.0f );

    // step = input frames advanced per output frame ( == 1/ratio ).
    double step = (double) srcRate / (double) targetRate_;
    std::vector<sample_t> in( (size_t) srcFrames );
    for( idx_t c = 0; c < channels_; ++c ) {
        // Pull the whole channel from the (resident) source once.
        src.read( 0, in.data(), srcFrames, c );
        sample_t *out = data_.data() + (size_t) c * nFrames_;
        for( length_t i = 0; i < nFrames_; ++i ) {
            double   sp   = (double) i * step;
            length_t k    = (length_t) sp;
            double   frac = sp - (double) k;
            sample_t a = ( k < srcFrames )       ? in[(size_t) k]       : 0.0f;
            sample_t b = ( k + 1 < srcFrames )   ? in[(size_t) ( k+1 )] : a;
            out[i] = (sample_t) ( a + ( b - a ) * frac );
        }
    }
}

twResampledSource::~twResampledSource()
{
}

length_t twResampledSource::read( offset_t srcOffset, sample_t *dest,
                                  length_t len, idx_t channel ) const
{
    if( len <= 0 ) return 0;
    if( nFrames_ <= 0 || channels_ <= 0 ) {
        memset( dest, 0, sizeof( sample_t ) * len );
        return 0;
    }

    idx_t ch = channel;
    if( ch < 0 ) ch = 0;
    if( ch >= channels_ ) ch = channels_ - 1;

    length_t avail = 0;
    if( srcOffset < (offset_t) nFrames_ ) {
        avail = nFrames_ - (length_t) srcOffset;
    }
    length_t n = len;
    if( n > avail ) n = avail;
    if( n < 0 ) n = 0;

    if( n > 0 ) {
        const sample_t *src = data_.data() + (size_t) ch * nFrames_ + srcOffset;
        memcpy( dest, src, sizeof( sample_t ) * n );
    }
    if( n < len ) {
        memset( dest + n, 0, sizeof( sample_t ) * ( len - n ) );
    }
    return n;
}

