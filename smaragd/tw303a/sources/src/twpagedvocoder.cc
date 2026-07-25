#include "tw/sources/twpagedvocoder.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// The in-house phase vocoder — M3 core, M4 keyframed random access,
// prominence-gated locking and LUT/table performance work. See the header
// for the algorithm contract. Everything is double-precision math over
// float-stored analysis, fixed evaluation order, single-threaded.

namespace {

constexpr double kPi    = 3.14159265358979323846;
constexpr double kTwoPi = 6.28318530717958647692;

inline double princarg( double p )
{
    p = std::fmod( p + kPi, kTwoPi );
    if( p <= 0.0 ) p += kTwoPi;
    return p - kPi;
}

double besselI0( double x )
{
    double sum = 1.0, term = 1.0;
    for( int k = 1; k < 32; k++ ) {
        term *= ( x / ( 2.0 * k ) ) * ( x / ( 2.0 * k ) );
        sum += term;
        if( term < 1e-18 * sum ) break;
    }
    return sum;
}

// Precomputed-table iterative radix-2 complex FFT (one size per instance).
struct FftPlan {
    uint32_t              n = 0;
    std::vector<uint32_t> rev;
    std::vector<double>   twRe, twIm;   // per stage, concatenated

    void init( uint32_t size )
    {
        n = size;
        rev.resize( n );
        rev[0] = 0;
        for( uint32_t i = 1; i < n; i++ )
            rev[i] = ( rev[i >> 1] >> 1 ) | ( ( i & 1 ) ? ( n >> 1 ) : 0 );
        twRe.clear(); twIm.clear();
        for( uint32_t len = 2; len <= n; len <<= 1 )
            for( uint32_t k = 0; k < len / 2; k++ ) {
                const double a = -kTwoPi * k / len;
                twRe.push_back( std::cos( a ) );
                twIm.push_back( std::sin( a ) );
            }
    }

    // inverse=false: forward. inverse=true: conjugate-symmetric inverse with
    // 1/n normalization.
    void run( double *re, double *im, bool inverse ) const
    {
        for( uint32_t i = 0; i < n; i++ ) {
            const uint32_t j = rev[i];
            if( i < j ) { std::swap( re[i], re[j] ); std::swap( im[i], im[j] ); }
        }
        size_t tw = 0;
        for( uint32_t len = 2; len <= n; len <<= 1 ) {
            for( uint32_t i = 0; i < n; i += len ) {
                for( uint32_t k = 0; k < len / 2; k++ ) {
                    const double cr = twRe[tw + k];
                    const double ci = inverse ? -twIm[tw + k] : twIm[tw + k];
                    const uint32_t a = i + k, b = i + k + len / 2;
                    const double tr = re[b] * cr - im[b] * ci;
                    const double ti = re[b] * ci + im[b] * cr;
                    re[b] = re[a] - tr;  im[b] = im[a] - ti;
                    re[a] += tr;         im[a] += ti;
                }
            }
            tw += len / 2;
        }
        if( inverse ) {
            const double s = 1.0 / (double) n;
            for( uint32_t i = 0; i < n; i++ ) { re[i] *= s; im[i] *= s; }
        }
    }
};

} // namespace

// ===========================================================================
// Impl
// ===========================================================================
struct twPagedVocoder::Impl {
    Config   cfg;
    std::vector<const float *> src;   // borrowed channel pointers
    uint64_t inLen = 0;
    double   stretch = 1.0, pitchRatio = 1.0, S = 1.0;
    uint64_t Ls = 0;                  // stretched-domain length
    uint32_t N = 0, Ha = 0, Hs = 0, nBins = 0;
    uint64_t aFrames = 0;             // analysis frame count
    uint64_t nSyn = 0;                // synthesis frame count over Ls

    FftPlan             fft;
    std::vector<double> win;

    // Lazy analysis cache (float storage): fold magnitude+phase, per-channel
    // complex spectra. haveFrame_ guards per-frame computation.
    std::vector<uint8_t> haveFrame;
    std::vector<float>   foldMag, foldPhase;
    std::vector<float>   chRe, chIm;

    // Sorted unique keyframe synthesis-frame indices (always contains 0).
    std::vector<uint64_t> keys;

    // Transient-preserving time map (M5): piecewise-linear OUTPUT→INPUT
    // analysis-position map. Around each onset the map runs at RATE 1 (no
    // stretch), so an attack keeps its exact waveform evolution instead of
    // being spread across repeated analysis frames — the smear fix phase
    // resets alone could not deliver. Zone anchors sit at o·S, so musical
    // timing is untouched; the steady spans between zones absorb the global
    // stretch. Strictly increasing in both domains by construction, built
    // once in init() from (onsets, S) — window-independent, so the paged ≡
    // whole partition property is preserved.
    std::vector<double> mapOut, mapIn;

    // Kaiser window LUT for the pitch resampler.
    static constexpr int    kSincTaps = 16;    // per side
    static constexpr double kBeta     = 8.0;
    std::vector<double>     kaiserLut;         // u in [0,1], linear interp

    // ---- scratch reused across frames (allocation-free inner loop) -------
    std::vector<double> fr, fi;
    std::vector<double> synPhase, zRe, zIm, mag, medScratch;
    std::vector<int>    peakOf;
    std::vector<uint8_t> locked;

    void init( const float *const *inSrc, uint64_t len, const Config &c,
               double st, double pr, const uint64_t *onsets, size_t nOnsets )
    {
        cfg = c;
        inLen = len;
        stretch = ( st > 0.0 ) ? st : 1e-6;
        pitchRatio = ( pr > 0.0 ) ? pr : 1.0;
        S  = stretch * pitchRatio;
        N  = cfg.fftSize;
        Ha = cfg.analysisHop;
        Hs = Ha;
        nBins = N / 2 + 1;
        Ls = std::max<uint64_t>( 1, (uint64_t) std::llround( (double) inLen * S ) );

        src.assign( inSrc, inSrc + cfg.channels );

        fft.init( N );
        win.resize( N );
        for( uint32_t j = 0; j < N; j++ )
            win[j] = 0.5 - 0.5 * std::cos( kTwoPi * j / ( N - 1 ) );

        aFrames = inLen ? ( ( inLen + Ha - 1 ) / Ha + 1 ) : 1;
        nSyn    = ( Ls + Hs - 1 ) / Hs + 1;

        haveFrame.assign( aFrames, 0 );
        foldMag.assign( aFrames * nBins, 0.0f );
        foldPhase.assign( aFrames * nBins, 0.0f );
        chRe.assign( aFrames * cfg.channels * nBins, 0.0f );
        chIm.assign( aFrames * cfg.channels * nBins, 0.0f );

        // Keyframe set: the fixed grid ∪ onset-aligned frames. Onset
        // positions arrive in the INPUT domain (already at this vocoder's
        // rate); an onset at input sample p lands at synthesis frame
        // round(p·S/Hs) — resetting there re-anchors the attack.
        keys.push_back( 0 );
        const uint32_t K = cfg.keyframeInterval ? cfg.keyframeInterval : 64;
        for( uint64_t k = K; k < nSyn; k += K ) keys.push_back( k );
        for( size_t i = 0; i < nOnsets; i++ ) {
            const uint64_t k =
                (uint64_t) std::llround( (double) onsets[i] * S / Hs );
            if( k < nSyn ) keys.push_back( k );
        }
        std::sort( keys.begin(), keys.end() );
        keys.erase( std::unique( keys.begin(), keys.end() ), keys.end() );

        // Defense in depth (M4): keyframes closer together than the OLA
        // overlap span are guaranteed-destructive — every crossfade region
        // would carry a reset discontinuity — whatever upstream produced
        // them (the v1 onset detector overfired on steady material). Keep
        // the first of any cluster; the fixed grid is far coarser than this.
        const uint64_t minGap = N / Hs;   // overlap depth (4 at 2048/512)
        std::vector<uint64_t> spaced;
        spaced.reserve( keys.size() );
        for( uint64_t k : keys )
            if( spaced.empty() || k - spaced.back() >= minGap )
                spaced.push_back( k );
        keys.swap( spaced );

        // --- Transient-preserving time map ---------------------------------
        // Active only when STRETCHING (S > 1): under compression, rate-1
        // protection makes attacks carry disproportionate energy vs the
        // shortened surroundings (+83% RMS measured on the transients
        // corpus) and the semantics are murky anyway — compression already
        // sharpens transients naturally. Stretching is where smear lives,
        // and there the map cuts rise-time ratios from ~2-4x to ~0.6-1.3x.
        mapOut.push_back( 0.0 );
        mapIn.push_back( 0.0 );
        if( S > 1.0 ) {
            const double P = (double) N;   // protection width, input samples
            double lastOut = 0.0, lastIn = 0.0;
            for( size_t i = 0; i < nOnsets; i++ ) {
                const double o = (double) onsets[i];
                if( o <= 0.0 || o >= (double) inLen ) continue;
                // ASYMMETRIC zone: the v2 detector marks onsets up to a
                // window EARLY (the earliest frame touching the attack wins
                // the normalized-flux race), so the attack body lies AFTER
                // the detected position — protect 1/4 window before, 3/4
                // after, or the burst tail falls outside the rate-1 zone and
                // smears (measured: outlier pairs at 3-4x with symmetric
                // zones, gone with asymmetric).
                double zInA  = o - P * 0.25, zInB  = o + P * 0.75;
                double zOutA = o * S - P * 0.25, zOutB = o * S + P * 0.75;
                // Monotonicity in BOTH domains: shift the zone start past the
                // previous segment end (shrinking the leading half) — never
                // let either domain step backwards.
                const double lift = std::max(
                    { lastIn - zInA + 1.0, lastOut - zOutA + 1.0, 0.0 } );
                zInA += lift;
                zOutA += lift;
                if( zInA >= zInB || zOutA >= zOutB ) continue;   // swallowed
                if( zInB >= (double) inLen || zOutB >= (double) Ls ) continue;
                mapOut.push_back( zOutA ); mapIn.push_back( zInA );
                mapOut.push_back( zOutB ); mapIn.push_back( zInB );
                lastOut = zOutB;
                lastIn  = zInB;
            }
        }
        mapOut.push_back( (double) Ls );
        mapIn.push_back( (double) inLen );

        // Phase re-anchors exactly where the map's rate switches: every
        // interior breakpoint (zone edge) becomes a keyframe, so no OLA
        // crossfade straddles a rate change with stale phase state.
        for( size_t j = 1; j + 1 < mapOut.size(); j++ ) {
            const uint64_t k = (uint64_t) std::llround( mapOut[j] / Hs );
            if( k < nSyn ) keys.push_back( k );
        }
        std::sort( keys.begin(), keys.end() );
        keys.erase( std::unique( keys.begin(), keys.end() ), keys.end() );
        {
            const uint64_t minGap2 = N / Hs;
            std::vector<uint64_t> spaced;
            spaced.reserve( keys.size() );
            for( uint64_t k : keys )
                if( spaced.empty() || k - spaced.back() >= minGap2 )
                    spaced.push_back( k );
            keys.swap( spaced );
        }

        kaiserLut.resize( 4097 );
        const double i0b = besselI0( kBeta );
        for( size_t i = 0; i < kaiserLut.size(); i++ ) {
            const double u = (double) i / ( kaiserLut.size() - 1 );
            kaiserLut[i] = besselI0( kBeta * std::sqrt( 1.0 - u * u ) ) / i0b;
        }

        fr.resize( N ); fi.resize( N );
        synPhase.assign( nBins, 0.0 );
        zRe.resize( nBins ); zIm.resize( nBins );
        mag.resize( nBins ); medScratch.resize( nBins );
        peakOf.resize( nBins );
        locked.resize( nBins );
    }

    // Piecewise-linear analysis position for stretched-output sample sOut.
    double mapAt( double sOut ) const
    {
        const size_t hi = (size_t) ( std::upper_bound( mapOut.begin(),
                                                       mapOut.end(), sOut )
                                     - mapOut.begin() );
        if( hi == 0 ) return mapIn.front();
        if( hi >= mapOut.size() ) return mapIn.back();
        const size_t lo = hi - 1;
        const double t = ( sOut - mapOut[lo] ) / ( mapOut[hi] - mapOut[lo] );
        return mapIn[lo] + t * ( mapIn[hi] - mapIn[lo] );
    }

    double kaiser( double u ) const
    {
        // u in [0,1]; linear interpolation over the LUT (deterministic).
        const double x = u * ( kaiserLut.size() - 1 );
        const size_t i = (size_t) x;
        if( i + 1 >= kaiserLut.size() ) return kaiserLut.back();
        const double f = x - (double) i;
        return kaiserLut[i] + ( kaiserLut[i + 1] - kaiserLut[i] ) * f;
    }

    // Compute (once) the analysis of frame f: fold mag/phase + per-channel
    // complex spectra. atan2 happens HERE, once per frame per bin — never in
    // the synthesis loop (the M3 performance sin).
    void ensureFrame( uint64_t f )
    {
        if( haveFrame[f] ) return;
        const int64_t start = (int64_t) f * Ha - (int64_t) N / 2;

        // Channels first (also feeds the fold accumulation).
        std::vector<double> foldR( N, 0.0 ), foldI( N, 0.0 );
        for( uint32_t c = 0; c < cfg.channels; c++ ) {
            for( uint32_t j = 0; j < N; j++ ) {
                const int64_t p = start + (int64_t) j;
                const double v =
                    ( p >= 0 && p < (int64_t) inLen ) ? (double) src[c][p] : 0.0;
                fr[j] = v * win[j];
                fi[j] = 0.0;
            }
            for( uint32_t j = 0; j < N; j++ ) foldR[j] += fr[j];
            fft.run( fr.data(), fi.data(), false );
            float *dre = chRe.data() + ( f * cfg.channels + c ) * nBins;
            float *dim = chIm.data() + ( f * cfg.channels + c ) * nBins;
            for( uint32_t b = 0; b < nBins; b++ ) {
                dre[b] = (float) fr[b];
                dim[b] = (float) fi[b];
            }
        }
        const double inv = 1.0 / cfg.channels;
        for( uint32_t j = 0; j < N; j++ ) { fr[j] = foldR[j] * inv; fi[j] = 0.0; }
        fft.run( fr.data(), fi.data(), false );
        float *fm = foldMag.data()   + f * nBins;
        float *fp = foldPhase.data() + f * nBins;
        for( uint32_t b = 0; b < nBins; b++ ) {
            fm[b] = (float) std::hypot( fr[b], fi[b] );
            fp[b] = (float) std::atan2( fi[b], fr[b] );
        }
        haveFrame[f] = 1;
    }

    // Advance the lock/phase state to synthesis frame k and produce the
    // rotation field zRe/zIm for it. Caller guarantees frames are visited in
    // ascending order starting from a keyframe.
    void stepFrame( uint64_t k )
    {
        const double  aPos = mapAt( (double) k * Hs );
        const double  fIdx = aPos / Ha;
        uint64_t f0 = (uint64_t) fIdx;
        if( f0 >= aFrames ) f0 = aFrames - 1;
        const uint64_t f1 = std::min( f0 + 1, aFrames - 1 );
        const uint64_t fN = ( fIdx - (double) f0 > 0.5 && f1 > f0 ) ? f1 : f0;
        ensureFrame( f0 );
        ensureFrame( f1 );
        ensureFrame( fN );

        const float *nm = foldMag.data()   + fN * nBins;
        const float *np = foldPhase.data() + fN * nBins;

        const bool isKey = std::binary_search( keys.begin(), keys.end(), k );

        if( isKey ) {
            // Keyframe: reset the whole phase state to the analysis truth.
            for( uint32_t b = 0; b < nBins; b++ ) synPhase[b] = np[b];
        } else {
            const float *p0 = foldPhase.data() + f0 * nBins;
            const float *p1 = foldPhase.data() + f1 * nBins;
            const double binFreq = kTwoPi / (double) N;
            for( uint32_t b = 0; b < nBins; b++ ) {
                const double omegaB = (double) b * binFreq;
                const double dev = ( f1 > f0 )
                    ? princarg( (double) p1[b] - (double) p0[b] - omegaB * Ha )
                          / (double) Ha
                    : 0.0;
                synPhase[b] += ( omegaB + dev ) * (double) Hs;
            }
        }

        for( uint32_t b = 0; b < nBins; b++ ) mag[b] = nm[b];

        // Frame median magnitude — the prominence yardstick.
        std::copy( mag.begin(), mag.end(), medScratch.begin() );
        std::nth_element( medScratch.begin(),
                          medScratch.begin() + nBins / 2, medScratch.end() );
        const double median = medScratch[nBins / 2];
        const double promThreshold = cfg.peakProminence * median;

        // PROMINENT peaks only (M4): spurious noise-floor maxima must not
        // impose phase structure — that was the comb/metallic coloration on
        // stochastic material. Bins below the median free-run regardless.
        std::vector<int> peaks;
        for( uint32_t b = 2; b + 2 < nBins; b++ ) {
            const double m = mag[b];
            if( m > mag[b-1] && m > mag[b-2] && m >= mag[b+1] && m >= mag[b+2]
                && m >= promThreshold && m > 0.0 )
                peaks.push_back( (int) b );
        }
        if( peaks.empty() ) {
            for( uint32_t b = 0; b < nBins; b++ ) { peakOf[b] = (int) b; locked[b] = 0; }
        } else {
            size_t pi = 0;
            for( uint32_t b = 0; b < nBins; b++ ) {
                while( pi + 1 < peaks.size()
                       && std::abs( (int) b - peaks[pi+1] )
                          <= std::abs( (int) b - peaks[pi] ) ) pi++;
                peakOf[b] = peaks[pi];
                locked[b] = ( mag[b] >= median ) ? 1 : 0;
            }
        }

        for( uint32_t b = 0; b < nBins; b++ ) {
            const double phiA = np[b];
            double phiOut;
            if( locked[b] && peakOf[b] != (int) b ) {
                const int p = peakOf[b];
                phiOut = synPhase[p] + ( phiA - (double) np[p] );
            } else {
                phiOut = synPhase[b];   // peak bins and free-running bins
            }
            const double rot = phiOut - phiA;
            zRe[b] = std::cos( rot );
            zIm[b] = std::sin( rot );
            synPhase[b] = phiOut;   // continuous state across role changes
        }
    }

    // Render the STRETCHED-domain window [s0, s1) into dst (planar,
    // per-channel stride = s1-s0). dst must be zeroed. Bit-exact under
    // partition: identical frame set, phase state and accumulation order for
    // any window covering a given output sample.
    void renderStretched( uint64_t s0, uint64_t s1, double *dst )
    {
        if( s1 <= s0 ) return;
        const uint64_t len = s1 - s0;
        const int64_t half = (int64_t) N / 2;

        // Frames whose window [k·Hs − N/2, k·Hs + N/2) intersects [s0, s1).
        int64_t kMin = ( (int64_t) s0 - half + (int64_t) Hs ) / (int64_t) Hs - 1;
        if( kMin < 0 ) kMin = 0;
        while( (int64_t) kMin * Hs + half <= (int64_t) s0 ) kMin++;
        int64_t kMax = ( (int64_t) s1 + half ) / (int64_t) Hs;
        if( kMax >= (int64_t) nSyn ) kMax = (int64_t) nSyn - 1;

        if( kMax < kMin ) return;

        // Pre-roll from the governing keyframe (phase state only, no OLA).
        auto it = std::upper_bound( keys.begin(), keys.end(), (uint64_t) kMin );
        const uint64_t kf = ( it == keys.begin() ) ? 0 : *( it - 1 );

        std::vector<double> norm( len, 0.0 );

        for( uint64_t k = kf; k <= (uint64_t) kMax; k++ ) {
            stepFrame( k );
            if( (int64_t) k < kMin ) continue;   // pre-roll only

            const int64_t base = (int64_t) k * Hs - half;
            // Window-square normalization contribution.
            for( uint32_t j = 0; j < N; j++ ) {
                const int64_t p = base + (int64_t) j;
                if( p < (int64_t) s0 || p >= (int64_t) s1 ) continue;
                norm[(size_t) ( p - (int64_t) s0 )] += win[j] * win[j];
            }
            const double aPos = mapAt( (double) k * Hs );
            const double fIdx = aPos / Ha;
            uint64_t f0 = (uint64_t) fIdx;
            if( f0 >= aFrames ) f0 = aFrames - 1;
            const uint64_t f1 = std::min( f0 + 1, aFrames - 1 );
            const uint64_t fN = ( fIdx - (double) f0 > 0.5 && f1 > f0 ) ? f1 : f0;

            for( uint32_t c = 0; c < cfg.channels; c++ ) {
                const float *cr = chRe.data() + ( fN * cfg.channels + c ) * nBins;
                const float *ci = chIm.data() + ( fN * cfg.channels + c ) * nBins;
                for( uint32_t b = 0; b < nBins; b++ ) {
                    fr[b] = (double) cr[b] * zRe[b] - (double) ci[b] * zIm[b];
                    fi[b] = (double) cr[b] * zIm[b] + (double) ci[b] * zRe[b];
                }
                for( uint32_t b = nBins; b < N; b++ ) {
                    fr[b] =  fr[N - b];
                    fi[b] = -fi[N - b];
                }
                fft.run( fr.data(), fi.data(), true );
                double *d = dst + (size_t) c * len;
                for( uint32_t j = 0; j < N; j++ ) {
                    const int64_t p = base + (int64_t) j;
                    if( p < (int64_t) s0 || p >= (int64_t) s1 ) continue;
                    d[(size_t) ( p - (int64_t) s0 )] += fr[j] * win[j];
                }
            }
        }

        for( uint32_t c = 0; c < cfg.channels; c++ ) {
            double *d = dst + (size_t) c * len;
            for( uint64_t i = 0; i < len; i++ )
                d[i] = ( norm[(size_t) i] > 1e-9 ) ? d[i] / norm[(size_t) i] : 0.0;
        }
    }

    void render( uint64_t outStart, uint64_t len, float *dst )
    {
        if( len == 0 || inLen == 0 || cfg.channels == 0 ) return;

        if( stretch == 1.0 && pitchRatio == 1.0 ) {
            // Identity: copy through exactly.
            for( uint32_t c = 0; c < cfg.channels; c++ ) {
                float *d = dst + (size_t) c * len;
                for( uint64_t i = 0; i < len; i++ ) {
                    const uint64_t p = outStart + i;
                    d[i] = ( p < inLen ) ? src[c][p] : 0.0f;
                }
            }
            return;
        }

        if( pitchRatio == 1.0 ) {
            const uint64_t s0 = std::min( outStart, Ls );
            const uint64_t s1 = std::min( outStart + len, Ls );
            if( s1 <= s0 ) return;
            std::vector<double> tmp( (size_t) cfg.channels * ( s1 - s0 ), 0.0 );
            renderStretched( s0, s1, tmp.data() );
            for( uint32_t c = 0; c < cfg.channels; c++ ) {
                float *d = dst + (size_t) c * len;
                const double *t = tmp.data() + (size_t) c * ( s1 - s0 );
                for( uint64_t i = 0; i < s1 - s0; i++ )
                    d[( s0 - outStart ) + i] = (float) t[i];
            }
            return;
        }

        // Pitch stage: output frame i reads stretched positions
        // (outStart+i)·ratio ± taps. Render exactly that stretched window,
        // then sinc-interpolate per output frame — each output sample
        // depends only on its bounded neighborhood, so partitions agree.
        const double ratio = pitchRatio;
        const double first = (double) outStart * ratio;
        const double last  = (double) ( outStart + len - 1 ) * ratio;
        int64_t s0 = (int64_t) std::floor( first ) - kSincTaps;
        int64_t s1 = (int64_t) std::floor( last ) + kSincTaps + 2;
        if( s0 < 0 ) s0 = 0;
        if( s1 > (int64_t) Ls ) s1 = (int64_t) Ls;
        if( s1 <= s0 ) return;
        const uint64_t sLen = (uint64_t) ( s1 - s0 );

        std::vector<double> tmp( (size_t) cfg.channels * sLen, 0.0 );
        renderStretched( (uint64_t) s0, (uint64_t) s1, tmp.data() );

        const double cutoff = 0.95 * std::min( 1.0, 1.0 / ratio );
        for( uint32_t c = 0; c < cfg.channels; c++ ) {
            const double *t = tmp.data() + (size_t) c * sLen;
            float *d = dst + (size_t) c * len;
            for( uint64_t i = 0; i < len; i++ ) {
                const double srcPos = (double) ( outStart + i ) * ratio;
                const int64_t k0 = (int64_t) std::floor( srcPos );
                double acc = 0.0, wsum = 0.0;
                for( int64_t k = k0 - kSincTaps + 1; k <= k0 + kSincTaps; k++ ) {
                    const double tt = srcPos - (double) k;
                    const double u = std::fabs( tt ) / (double) kSincTaps;
                    if( u >= 1.0 ) continue;
                    const double sx = kPi * cutoff * tt;
                    const double sinc = ( std::fabs( sx ) < 1e-12 )
                        ? 1.0 : std::sin( sx ) / sx;
                    const double w = cutoff * sinc * kaiser( u );
                    const double v = ( k >= s0 && k < s1 )
                        ? t[(size_t) ( k - s0 )] : 0.0;
                    acc  += v * w;
                    wsum += w;
                }
                d[i] = ( std::fabs( wsum ) > 1e-9 ) ? (float) ( acc / wsum ) : 0.0f;
            }
        }
    }
};

// ===========================================================================
// Public surface
// ===========================================================================
twPagedVocoder::twPagedVocoder( const float *const *src, uint64_t inLen,
                                const Config &cfg, double stretch,
                                double pitchRatio, const uint64_t *onsets,
                                size_t nOnsets )
    : impl_( new Impl )
{
    impl_->init( src, inLen, cfg, stretch, pitchRatio, onsets, nOnsets );
}

twPagedVocoder::~twPagedVocoder() = default;

void twPagedVocoder::render( uint64_t outStart, uint64_t len, float *dst )
{
    impl_->render( outStart, len, dst );
}

void twPagedVocoder::warpOffline( const float *const *in, uint64_t inLen,
                                  float *out, uint64_t outLen,
                                  const Config &cfg, double stretch,
                                  double pitchRatio, const uint64_t *onsets,
                                  size_t nOnsets )
{
    if( !out || outLen == 0 ) return;
    std::memset( out, 0, sizeof( float ) * (size_t) cfg.channels * outLen );
    if( !in || inLen == 0 || cfg.channels == 0 ) return;
    twPagedVocoder v( in, inLen, cfg, stretch, pitchRatio, onsets, nOnsets );
    v.render( 0, outLen, out );
}
