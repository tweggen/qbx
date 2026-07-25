#include "tw/sources/twpagedvocoder.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

// The in-house phase vocoder (proposal 27 M3). See the header for the
// algorithm overview. Everything here is double-precision, fixed-order,
// single-threaded — determinism is a gate. The FFT is a plain iterative
// radix-2 (adequate for the offline builder; pffft/SIMD arrive with M4's
// random-access work where per-page throughput matters).

namespace {

constexpr double kPi    = 3.14159265358979323846;
constexpr double kTwoPi = 6.28318530717958647692;

// ---------------------------------------------------------------------------
// Iterative radix-2 complex FFT, in-place, double precision.
// ---------------------------------------------------------------------------
void fftInPlace( std::vector<double> &re, std::vector<double> &im, bool inverse )
{
    const size_t n = re.size();
    // Bit-reversal permutation.
    for( size_t i = 1, j = 0; i < n; i++ ) {
        size_t bit = n >> 1;
        for( ; j & bit; bit >>= 1 ) j ^= bit;
        j ^= bit;
        if( i < j ) {
            std::swap( re[i], re[j] );
            std::swap( im[i], im[j] );
        }
    }
    for( size_t len = 2; len <= n; len <<= 1 ) {
        const double ang = ( inverse ? kTwoPi : -kTwoPi ) / (double) len;
        const double wr = std::cos( ang ), wi = std::sin( ang );
        for( size_t i = 0; i < n; i += len ) {
            double cr = 1.0, ci = 0.0;
            for( size_t k = 0; k < len / 2; k++ ) {
                const size_t a = i + k, b = i + k + len / 2;
                const double tr = re[b] * cr - im[b] * ci;
                const double ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr;  im[b] = im[a] - ti;
                re[a] += tr;         im[a] += ti;
                const double ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
    if( inverse ) {
        const double s = 1.0 / (double) n;
        for( size_t i = 0; i < n; i++ ) { re[i] *= s; im[i] *= s; }
    }
}

inline double princarg( double p )
{
    // Wrap to (-pi, pi].
    p = std::fmod( p + kPi, kTwoPi );
    if( p <= 0.0 ) p += kTwoPi;
    return p - kPi;
}

// ---------------------------------------------------------------------------
// STFT analysis of one channel: complex frames at fixed hop, Hann window,
// frames zero-padded past either end of the material.
// ---------------------------------------------------------------------------
struct Stft {
    uint32_t n      = 0;   // fftSize
    uint32_t hop    = 0;
    uint32_t nBins  = 0;   // n/2 + 1
    uint64_t frames = 0;
    // Frame-major: frame f, bin b at [f*nBins + b].
    std::vector<double> re, im;
};

void analyze( const float *x, uint64_t len, uint32_t n, uint32_t hop,
              const std::vector<double> &win, Stft &out )
{
    out.n     = n;
    out.hop   = hop;
    out.nBins = n / 2 + 1;
    out.frames = len ? ( ( len + hop - 1 ) / hop + 1 ) : 1;
    out.re.assign( out.frames * out.nBins, 0.0 );
    out.im.assign( out.frames * out.nBins, 0.0 );

    std::vector<double> fr( n ), fi( n );
    for( uint64_t f = 0; f < out.frames; f++ ) {
        const int64_t start = (int64_t) f * hop - (int64_t) n / 2;
        for( uint32_t j = 0; j < n; j++ ) {
            const int64_t p = start + (int64_t) j;
            const double v = ( p >= 0 && p < (int64_t) len ) ? (double) x[p] : 0.0;
            fr[j] = v * win[j];
            fi[j] = 0.0;
        }
        fftInPlace( fr, fi, false );
        double *dre = out.re.data() + f * out.nBins;
        double *dim = out.im.data() + f * out.nBins;
        for( uint32_t b = 0; b < out.nBins; b++ ) { dre[b] = fr[b]; dim[b] = fi[b]; }
    }
}

// ---------------------------------------------------------------------------
// Kaiser-windowed sinc resampler (pitch stage). ratio > 1 shortens/pitches up.
// ---------------------------------------------------------------------------
double besselI0( double x )
{
    // Series expansion; converges fast for the beta range used here.
    double sum = 1.0, term = 1.0;
    for( int k = 1; k < 32; k++ ) {
        term *= ( x / ( 2.0 * k ) ) * ( x / ( 2.0 * k ) );
        sum += term;
        if( term < 1e-18 * sum ) break;
    }
    return sum;
}

void resampleSinc( const std::vector<double> &in, double ratio,
                   std::vector<double> &out, uint64_t outLen )
{
    out.assign( outLen, 0.0 );
    if( in.empty() ) return;
    const int    taps   = 16;               // per side
    const double beta   = 8.0;
    const double i0b    = besselI0( beta );
    const double cutoff = 0.95 * std::min( 1.0, 1.0 / ratio );
    for( uint64_t i = 0; i < outLen; i++ ) {
        const double srcPos = (double) i * ratio;
        const int64_t k0 = (int64_t) std::floor( srcPos );
        double acc = 0.0, wsum = 0.0;
        for( int64_t k = k0 - taps + 1; k <= k0 + taps; k++ ) {
            const double t = srcPos - (double) k;          // |t| <= taps
            const double u = t / (double) taps;            // [-1, 1]
            if( u <= -1.0 || u >= 1.0 ) continue;
            const double kaiser = besselI0( beta * std::sqrt( 1.0 - u * u ) ) / i0b;
            const double sx = kPi * cutoff * t;
            const double sinc = ( std::fabs( sx ) < 1e-12 ) ? 1.0
                                                            : std::sin( sx ) / sx;
            const double w = cutoff * sinc * kaiser;
            const double v = ( k >= 0 && k < (int64_t) in.size() ) ? in[k] : 0.0;
            acc  += v * w;
            wsum += w;
        }
        // Normalize by the window sum so pass-band gain stays unity even at
        // the clip edges where taps fall outside the material.
        out[i] = ( std::fabs( wsum ) > 1e-9 ) ? acc / wsum : 0.0;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// The offline warp.
// ---------------------------------------------------------------------------
void twPagedVocoder::warpOffline( const float *const *in, uint64_t inLen,
                                  float *out, uint64_t outLen,
                                  const Config &cfg,
                                  double stretch, double pitchRatio )
{
    if( !out || outLen == 0 ) return;
    std::memset( out, 0, sizeof( float ) * (size_t) cfg.channels * outLen );
    if( !in || inLen == 0 || cfg.channels == 0 ) return;
    if( stretch <= 0.0 ) stretch = 1e-6;
    if( pitchRatio <= 0.0 ) pitchRatio = 1.0;

    const uint32_t N  = cfg.fftSize;
    const uint32_t Ha = cfg.analysisHop;
    const uint32_t Hs = Ha;                       // fixed synthesis hop
    const uint32_t nBins = N / 2 + 1;
    const double   S  = stretch * pitchRatio;     // internal time-stretch
    const uint64_t Ls = std::max<uint64_t>( 1, (uint64_t) std::llround( (double) inLen * S ) );

    // Identity short-circuit: no stretch, no pitch — copy through exactly.
    if( stretch == 1.0 && pitchRatio == 1.0 ) {
        const uint64_t nCopy = std::min( inLen, outLen );
        for( uint32_t c = 0; c < cfg.channels; c++ )
            for( uint64_t i = 0; i < nCopy; i++ )
                out[(size_t) c * outLen + i] = in[c][i];
        return;
    }

    std::vector<double> win( N );
    for( uint32_t j = 0; j < N; j++ )
        win[j] = 0.5 - 0.5 * std::cos( kTwoPi * j / ( N - 1 ) );

    // --- Analysis: every channel + the mono fold -------------------------
    std::vector<Stft> chan( cfg.channels );
    std::vector<float> mono( (size_t) inLen );
    for( uint64_t i = 0; i < inLen; i++ ) {
        double m = 0.0;
        for( uint32_t c = 0; c < cfg.channels; c++ ) m += in[c][i];
        mono[i] = (float) ( m / cfg.channels );
    }
    Stft fold;
    analyze( mono.data(), inLen, N, Ha, win, fold );
    for( uint32_t c = 0; c < cfg.channels; c++ )
        analyze( in[c], inLen, N, Ha, win, chan[c] );

    const uint64_t nSyn = ( Ls + Hs - 1 ) / Hs + 1;

    // --- Synthesis state --------------------------------------------------
    // Accumulated synthesis phase per bin (mono fold), and the per-frame
    // rotation Z[b] applied to every channel.
    std::vector<double> synPhase( nBins, 0.0 );
    std::vector<double> zRe( nBins ), zIm( nBins );
    std::vector<double> mag( nBins );
    std::vector<int>    peakOf( nBins );

    // Stretched-domain accumulators per channel + window-square normalizer.
    std::vector<std::vector<double>> acc( cfg.channels,
                                          std::vector<double>( (size_t) Ls + N, 0.0 ) );
    std::vector<double> norm( (size_t) Ls + N, 0.0 );

    std::vector<double> fr( N ), fi( N );

    const double binFreq = kTwoPi / (double) N;   // Ω_b factor: b * binFreq rad/sample

    for( uint64_t k = 0; k < nSyn; k++ ) {
        // Fractional analysis position for this synthesis frame.
        const double aPos   = (double) k * Hs / S;          // input samples
        const double fIdx   = aPos / Ha;                    // fractional frame
        uint64_t f0 = (uint64_t) std::floor( fIdx );
        if( f0 >= fold.frames ) f0 = fold.frames - 1;
        uint64_t f1 = std::min( f0 + 1, fold.frames - 1 );
        const uint64_t fN = ( fIdx - (double) f0 > 0.5 && f1 > f0 ) ? f1 : f0; // nearest

        const double *nr = fold.re.data() + fN * nBins;
        const double *ni = fold.im.data() + fN * nBins;

        for( uint32_t b = 0; b < nBins; b++ )
            mag[b] = std::hypot( nr[b], ni[b] );

        if( k == 0 ) {
            // Phase reset at the start: copy analysis phases verbatim.
            for( uint32_t b = 0; b < nBins; b++ )
                synPhase[b] = std::atan2( ni[b], nr[b] );
        } else {
            // Instantaneous frequency between f0 and f1 of the fold.
            const double *r0 = fold.re.data() + f0 * nBins;
            const double *i0 = fold.im.data() + f0 * nBins;
            const double *r1 = fold.re.data() + f1 * nBins;
            const double *i1 = fold.im.data() + f1 * nBins;
            for( uint32_t b = 0; b < nBins; b++ ) {
                const double p0 = std::atan2( i0[b], r0[b] );
                const double p1 = std::atan2( i1[b], r1[b] );
                const double omegaB = (double) b * binFreq;
                double dev;
                if( f1 > f0 ) {
                    dev = princarg( p1 - p0 - omegaB * Ha ) / (double) Ha;
                } else {
                    dev = 0.0;   // clamped at the end: pure bin frequency
                }
                synPhase[b] += ( omegaB + dev ) * (double) Hs;
            }
        }

        // Peak picking on the nearest-frame magnitudes (2-neighbor test),
        // then region assignment: every bin belongs to the closest peak.
        int lastPeak = -1;
        for( uint32_t b = 0; b < nBins; b++ ) peakOf[b] = -1;
        std::vector<int> peaks;
        for( uint32_t b = 2; b + 2 < nBins; b++ ) {
            const double m = mag[b];
            if( m > mag[b-1] && m > mag[b-2] && m >= mag[b+1] && m >= mag[b+2] )
                peaks.push_back( (int) b );
        }
        if( peaks.empty() ) {
            for( uint32_t b = 0; b < nBins; b++ ) peakOf[b] = (int) b; // self-locked
        } else {
            size_t pi = 0;
            for( uint32_t b = 0; b < nBins; b++ ) {
                while( pi + 1 < peaks.size()
                       && std::abs( (int) b - peaks[pi+1] )
                          <= std::abs( (int) b - peaks[pi] ) ) pi++;
                peakOf[b] = peaks[pi];
            }
            (void) lastPeak;
        }

        // Identity phase locking: peak bins carry the accumulated phase;
        // non-peak bins keep their analysis offset relative to their peak.
        // The per-bin ROTATION Z[b] = e^{i(φ_out[b] − φ_a[b])} is what gets
        // applied to every channel (cross-channel coherence).
        for( uint32_t b = 0; b < nBins; b++ ) {
            const int p = peakOf[b];
            const double phiA  = std::atan2( ni[b], nr[b] );
            const double phiAp = std::atan2( ni[p], nr[p] );
            const double phiOut = ( (int) b == p )
                ? synPhase[b]
                : synPhase[p] + ( phiA - phiAp );
            const double rot = phiOut - phiA;
            zRe[b] = std::cos( rot );
            zIm[b] = std::sin( rot );
            // EVERY bin records the phase it actually output — when peak
            // roles change hands between frames, the new peak's accumulated
            // state is then continuous with what was heard, not with a
            // never-output shadow accumulation.
            synPhase[b] = phiOut;
        }

        // Per channel: rotate the channel spectrum, iFFT, overlap-add.
        const uint64_t outPos = k * Hs;
        for( uint32_t c = 0; c < cfg.channels; c++ ) {
            const double *cr = chan[c].re.data() + fN * nBins;
            const double *ci = chan[c].im.data() + fN * nBins;
            for( uint32_t b = 0; b < nBins; b++ ) {
                fr[b] = cr[b] * zRe[b] - ci[b] * zIm[b];
                fi[b] = cr[b] * zIm[b] + ci[b] * zRe[b];
            }
            // Hermitian mirror for the real iFFT.
            for( uint32_t b = nBins; b < N; b++ ) {
                fr[b] =  fr[N - b];
                fi[b] = -fi[N - b];
            }
            fftInPlace( fr, fi, true );
            std::vector<double> &A = acc[c];
            for( uint32_t j = 0; j < N; j++ ) {
                const int64_t p = (int64_t) outPos + (int64_t) j - (int64_t) N / 2;
                if( p < 0 || p >= (int64_t) ( Ls + N ) ) continue;
                A[(size_t) p] += fr[j] * win[j];
            }
        }
        for( uint32_t j = 0; j < N; j++ ) {
            const int64_t p = (int64_t) outPos + (int64_t) j - (int64_t) N / 2;
            if( p < 0 || p >= (int64_t) ( Ls + N ) ) continue;
            norm[(size_t) p] += win[j] * win[j];
        }
    }

    // --- Normalize, resample for pitch, land exactly outLen ---------------
    std::vector<double> stretched( (size_t) Ls );
    for( uint32_t c = 0; c < cfg.channels; c++ ) {
        std::vector<double> &A = acc[c];
        for( uint64_t i = 0; i < Ls; i++ )
            stretched[(size_t) i] = ( norm[(size_t) i] > 1e-9 )
                ? A[(size_t) i] / norm[(size_t) i] : 0.0;

        float *dst = out + (size_t) c * outLen;
        if( pitchRatio == 1.0 ) {
            const uint64_t nCopy = std::min( Ls, outLen );
            for( uint64_t i = 0; i < nCopy; i++ )
                dst[i] = (float) stretched[(size_t) i];
        } else {
            std::vector<double> shifted;
            resampleSinc( stretched, pitchRatio, shifted, outLen );
            for( uint64_t i = 0; i < outLen; i++ )
                dst[i] = (float) shifted[(size_t) i];
        }
    }
}
