
#include <math.h>
#include <string.h>

#include "tw/sources/twgrainsource.h"

twGrainSource::twGrainSource( const twRandomSource &src, const twGrainParams &p )
    : rate_( src.sampleRate() ),
      channels_( src.channels() ),
      nFrames_( 0 ),
      reproducible_( src.isReproducible() )
{
    length_t inLen = src.length();
    if( channels_ <= 0 || inLen <= 0 ) {
        if( channels_ < 0 ) channels_ = 0;
        return;
    }

    // Output length is EXACT (proposal 18 Phase 2): floor(inLen * stretch)
    // computed rationally — the render-boundary rounding rule. The grain
    // scheduling below (hop spacing, per-sample interpolation) stays double:
    // it is synthesis-internal and never feeds back into position math.
    Fraction stretchFrac = ( p.stretch > Fraction(0) ) ? p.stretch
                                                       : Fraction(1, 1000000);
    double   stretch = stretchFrac.approxDouble();
    if( stretch < 1e-6 ) stretch = 1e-6;
    double   r       = pow( 2.0, p.pitchCents / 1200.0 );   // pitch ratio
    length_t G       = ( p.grainSize > 0 ) ? p.grainSize : 2048;
    length_t C       = p.crossfade;
    if( C < 0 ) C = 0;
    if( C > G / 2 ) C = G / 2;            // need G >= 2C for unity crossfade
    length_t Ho = G - C;                  // output hop
    if( Ho <= 0 ) Ho = 1;
    double   Hi = (double) Ho / stretch;  // input hop (frames)

    nFrames_ = (length_t) ( Fraction( inLen ) * stretchFrac ).floorToInt();
    if( nFrames_ <= 0 ) {
        nFrames_ = 0;
        return;
    }

    data_.assign( (size_t) channels_ * nFrames_, 0.0f );
    std::vector<sample_t> in( (size_t) inLen );
    // Window-weight accumulator, reused per channel: normalising by it makes the
    // overlap-add unity-gain everywhere there is coverage, regardless of how the
    // fades line up at clip edges.
    std::vector<float> wsum( (size_t) nFrames_ );

    for( idx_t c = 0; c < channels_; ++c ) {
        src.read( 0, in.data(), inLen, c );
        sample_t *out = data_.data() + (size_t) c * nFrames_;
        memset( wsum.data(), 0, sizeof( float ) * (size_t) nFrames_ );

        for( length_t g = 0; ; ++g ) {
            length_t outPos = g * Ho;
            if( outPos >= nFrames_ ) break;
            double inPos = (double) g * Hi;

            for( length_t j = 0; j < G; ++j ) {
                length_t op = outPos + j;
                if( op >= nFrames_ ) break;

                double   sp = inPos + (double) j * r;   // input position for this grain sample
                if( sp < 0.0 ) continue;
                length_t k    = (length_t) sp;
                double   frac = sp - (double) k;
                sample_t a = ( k < inLen )       ? in[(size_t) k]         : 0.0f;
                sample_t b = ( k + 1 < inLen )   ? in[(size_t) ( k + 1 )] : a;
                sample_t s = (sample_t) ( a + ( b - a ) * frac );

                double w;
                if( C > 0 && j < C )            w = (double) j / (double) C;          // fade in
                else if( C > 0 && j >= G - C )  w = (double) ( G - j ) / (double) C;  // fade out
                else                            w = 1.0;

                out[op]  += (sample_t) ( s * w );
                wsum[op] += (float) w;
            }
        }

        for( length_t i = 0; i < nFrames_; ++i ) {
            if( wsum[i] > 1e-4f ) out[i] /= (sample_t) wsum[i];
        }
    }
}

twGrainSource::~twGrainSource()
{
}

length_t twGrainSource::read( offset_t srcOffset, sample_t *dest,
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

    // A negative source offset (a slip anchor dragged before the material start)
    // would make `avail` exceed nFrames_ and the memcpy below read before the
    // buffer. The pre-roll is silence: emit zeros for the leading gap and start
    // the copy at frame 0.
    if( srcOffset < 0 ) {
        length_t gap = (length_t) ( -srcOffset );
        if( gap >= len ) { memset( dest, 0, sizeof( sample_t ) * len ); return 0; }
        memset( dest, 0, sizeof( sample_t ) * gap );
        return read( 0, dest + gap, len - gap, channel );
    }

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

