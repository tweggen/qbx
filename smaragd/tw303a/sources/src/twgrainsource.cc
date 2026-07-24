
#include <math.h>
#include <string.h>

#include "tw/sources/twgrainsource.h"

#if TW_HAVE_RUBBERBAND
#include <vector>
#include <rubberband/RubberBandStretcher.h>
#endif

// twGrainSource materialises a time-stretched / pitch-shifted copy of another
// twRandomSource ONCE, in this constructor, into a resident planar Float32
// buffer; read() is then a lock-free memcpy (see the header). Two synthesis
// backends live here, selected at build time:
//
//   * TW_HAVE_RUBBERBAND — Rubber Band Library (proposal 26), a phase-vocoder
//     engine, run in OFFLINE mode (study whole signal, then process/retrieve).
//     This is the shipping path; it removes the amplitude-modulation warble the
//     naive overlap-add produced on tonal material.
//   * otherwise           — the legacy fixed-hop time-slice overlap-add
//     (proposal 06), kept verbatim as a dependency-free fallback and reference.
//
// Whatever the backend, the output length is EXACT (proposal 18 Phase 2):
// nFrames_ = floor(inLen * stretch), computed rationally — the render-boundary
// rounding rule the cut window (WarpedLen domain) depends on. Rubber Band's
// own output count is only approximate, so its drained output is clamped /
// zero-padded to exactly nFrames_ before it lands in data_.

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

    Fraction stretchFrac = ( p.stretch > Fraction(0) ) ? p.stretch
                                                       : Fraction(1, 1000000);
    double   stretch = stretchFrac.approxDouble();
    if( stretch < 1e-6 ) stretch = 1e-6;
    double   r       = pow( 2.0, p.pitchCents / 1200.0 );   // pitch ratio

    nFrames_ = (length_t) ( Fraction( inLen ) * stretchFrac ).floorToInt();
    if( nFrames_ <= 0 ) {
        nFrames_ = 0;
        return;
    }

    data_.assign( (size_t) channels_ * nFrames_, 0.0f );

#if TW_HAVE_RUBBERBAND
    // --- Rubber Band offline (proposal 26) -----------------------------------
    // stretch is out/in duration → timeRatio; the pitch ratio r → pitchScale.
    // OptionThreadingNever keeps the offline pass single-threaded so the warp
    // is deterministic (the qxa flake gate and the byte-exact render cmp both
    // depend on that). OptionEngineFiner selects the R3 (higher quality) engine.
    // Formants are left at the default (they scale with pitch — no formant
    // preservation): preservation is a source-filter/vocal assumption that
    // colours and de-energises general material, so it is not a good global
    // default. A per-clip formant toggle can wire it back in for voice later.
    const size_t kBlock = 4096;   // input frames fed per process() call
    using RB = RubberBand::RubberBandStretcher;
    RB::Options opts = RB::OptionProcessOffline
                     | RB::OptionEngineFiner
                     | RB::OptionThreadingNever;
    RB rb( (size_t) rate_, (size_t) channels_, opts,
           /*timeRatio*/ stretch, /*pitchScale*/ r );
    rb.setDebugLevel( 0 );                 // no direct-stderr chatter (logging policy)
    rb.setExpectedInputDuration( (size_t) inLen );
    rb.setMaxProcessSize( kBlock );        // pre-size internal buffers to the block

    // Read the whole source, planar — one contiguous buffer per channel — and
    // present it to Rubber Band as float* const* (all channels together, which
    // keeps the stereo image phase-coherent; the legacy path stretched each
    // channel independently).
    std::vector<std::vector<float>> inCh( (size_t) channels_ );
    std::vector<const float*>       inPtr( (size_t) channels_ );
    for( idx_t c = 0; c < channels_; ++c ) {
        inCh[(size_t) c].resize( (size_t) inLen );
        src.read( 0, inCh[(size_t) c].data(), inLen, c );
        inPtr[(size_t) c] = inCh[(size_t) c].data();
    }

    rb.study( inPtr.data(), (size_t) inLen, true );

    // Feed the input in bounded blocks, draining the ready output after each so
    // Rubber Band's output ring never overflows. A single whole-clip process()
    // forces it to grow that buffer (noisy stderr warnings + reallocation); the
    // offline result is identical either way, this just drives it correctly.
    std::vector<std::vector<float>> outCh( (size_t) channels_ );
    std::vector<const float*>       blkIn( (size_t) channels_ );

    auto drain = [&]() {
        for( int avail; ( avail = rb.available() ) > 0; ) {
            std::vector<float*> outPtr( (size_t) channels_ );
            for( idx_t c = 0; c < channels_; ++c ) {
                size_t old = outCh[(size_t) c].size();
                outCh[(size_t) c].resize( old + (size_t) avail );
                outPtr[(size_t) c] = outCh[(size_t) c].data() + old;
            }
            rb.retrieve( outPtr.data(), (size_t) avail );
        }
    };

    for( length_t pos = 0; pos < inLen; pos += (length_t) kBlock ) {
        length_t n = inLen - pos;
        if( n > (length_t) kBlock ) n = (length_t) kBlock;
        for( idx_t c = 0; c < channels_; ++c )
            blkIn[(size_t) c] = inCh[(size_t) c].data() + pos;
        rb.process( blkIn.data(), (size_t) n, /*final=*/ pos + n >= inLen );
        drain();
    }
    drain();   // flush any remainder after the final block

    // Land EXACTLY nFrames_ per channel: truncate the tail or leave the
    // remainder as the zero-fill data_ was assigned with.
    for( idx_t c = 0; c < channels_; ++c ) {
        const std::vector<float> &o = outCh[(size_t) c];
        length_t n = (length_t) o.size();
        if( n > nFrames_ ) n = nFrames_;
        if( n > 0 )
            memcpy( data_.data() + (size_t) c * nFrames_, o.data(),
                    sizeof( sample_t ) * (size_t) n );
    }

    // NO output gain adjustment: Rubber Band's R3 resynthesis is loudness-
    // preserving, so a time-stretch or pitch-shift keeps the source RMS (a
    // longer clip carries proportionally more total energy — the level is what
    // stays constant). An earlier global peak-scaling "anti-clip" was removed:
    // it dimmed the WHOLE clip whenever a single Gibbs transient overshot,
    // which broke exactly that loudness invariance. Any rare overshoot on
    // near-full-scale material is clamped by the format conversion at the render
    // boundary, the same as for any other signal.
#else
    // --- Legacy time-slice overlap-add fallback (proposal 06) ----------------
    // Fixed grain size / crossfade, one stretch, one pitch. The grain
    // scheduling (hop spacing, per-sample interpolation) is synthesis-internal
    // double math and never feeds back into position math.
    length_t G = ( p.grainSize > 0 ) ? p.grainSize : 2048;
    length_t C = p.crossfade;
    if( C < 0 ) C = 0;
    if( C > G / 2 ) C = G / 2;            // need G >= 2C for unity crossfade
    length_t Ho = G - C;                  // output hop
    if( Ho <= 0 ) Ho = 1;
    double   Hi = (double) Ho / stretch;  // input hop (frames)

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
#endif
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

    // Leading silence for a read that starts before the material — same rule
    // (and same latent out-of-bounds without it) as twSampleSource::read.
    if( srcOffset < 0 ) {
        length_t lead = (length_t)( -srcOffset );
        if( lead > len ) lead = len;
        memset( dest, 0, sizeof( sample_t ) * lead );
        dest += lead;
        len  -= lead;
        srcOffset = 0;
        if( len <= 0 ) return 0;
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
