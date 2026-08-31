// body_probe -- what the puppet's motion actually IS, measured, not asserted.
//
// An offline probe (the analog of clap_probe / vst3_probe / asio_probe), NOT a
// gate: it runs the REAL shipped chain
//
//     twGrooveAnalyzeFrontEnd -> twGrooveBuildAspectPayloads
//         -> twGrooveDecode{Res,Dyn}Payload
//         -> the EXACT sFeelFlowPoseAt formula (energy * cosPhi, by unit NAME)
//
// over a committed groove fixture, and reports the KINEMATICS of the resulting
// five-part pose plus the forces that motion would demand of a real body. It
// exists because proposal 40's puppet was reported (2026-08-27) as "not
// honouring the physics of the human body -- the head is not subordinate to
// the torso, and the movement is linear, without weight or sudden stops", and
// this repo's rule is to MEASURE such a claim rather than argue about it.
//
// Read this before trusting a number out of it -- three things are deliberate:
//
//  1. **It reads the SHIPPED aspect payloads, not the trajectories directly.**
//     The puppet sees `sqrt(unitPower) * cosPhi` out of the decoded "groove.res"
//     and "groove.dyn" records, with cosPhi BIN-AVERAGED onto the 100 Hz aspect
//     grid. Measuring the pendulum's own `magnitude`/`phaseWrapped` instead
//     would measure a signal nothing draws, and would miss the bin-average's
//     own damping of an incoherent bin (sfeelflowpose.h point 2).
//
//  2. **Every "expected" figure printed beside a measurement is a CLOSED FORM
//     for a pure sinusoid**, not a taste judgement: dwell10% = (2/pi)*asin(0.1)
//     = 0.0638, crest factor of the acceleration = sqrt(2), skewness = 0,
//     kurtosis = 1.5. A pose component IS `A(t)*cos(phi(t))` by construction,
//     so those are the values the current model must produce once its envelope
//     A(t) is held still -- which is what section A2 does by restricting to the
//     longest stretch where the part's own energy stays above 60% of its run
//     peak. Section A (the whole run) and section A2 (steady stretches) answer
//     DIFFERENT questions and disagreeing is the point: A is dominated by the
//     envelope collapsing and recharging, A2 by the shape of one movement cycle.
//
//  3. **The fixture must be in the ensemble's HEALTHY regime or the numbers mean
//     nothing.** The seeded periods are all multiples of the recovered tatum, so
//     a fixture whose tatum comes back at the wrong metrical level puts every
//     unit at the wrong rate and the kinematics measure the mis-seed rather than
//     the model. The committed groove fixtures recover tatum = 0.25 s at 120 BPM
//     (reference 4 Hz / bounce 2 Hz / limbs 1 Hz / sway 0.5 Hz / twobar 0.25 Hz);
//     the probe PRINTS the recovered tatum and every unit's period first, and a
//     run whose header does not show that ladder should be discarded, not read.
//     (Measured while building this: a hand-written backbeat at 120 BPM -- kick
//     on 1 and 3, snare on 2 and 4, hats on eighths -- recovers tatum = 1.0 s,
//     four metrical levels too slow, because the kick-snare alternation is a
//     stronger autocorrelation peak than the hats. That is the octave/meter
//     ambiguity the design already names as genuinely hard, not a defect found
//     here; it is why this tool takes a fixture path rather than synthesising.)
//
// Usage:  body_probe <fixture.wav> [breakStartSec breakEndSec] [trace.csv]
//         body_probe <fixture.wav> --hash-payloads
//   e.g.  body_probe tests/groove/h_fill_break.wav 18.22 22.22
//
// --hash-payloads prints exactly three lines -- the SHA-256 of resPayload,
// evPayload and dynPayload -- and nothing else. That is proposal 44's AC-INV
// check ("the entrainment ensemble does not move"), kept separate from the
// human-readable sections precisely so those may change freely.
//
// Build:  the CMake target `body_probe` (tw303a/CMakeLists.txt), or -- because
//   it links tw_sidecar's three groove sources and nothing else, no Qt and no
//   audio file library -- with a bare compiler and no configure step at all:
//     sh smaragd/tw303a/sidecar/tools/build_body_probe.sh [outdir]

#include "tw/sidecar/twgroove.h"
#include "tw/sidecar/twgroovependulum.h"
#include "tw/sidecar/twgrooveaspect.h"
#include "tw/body/twbodymeasures.h"   // proposal 44 C2: the measures, not magic numbers

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

// ---- SHA-256, in-tree, because the probe links tw_sidecar and nothing else -
// Used ONLY by --hash-payloads. Deliberately not a crypto claim: this is a
// change detector for three byte buffers.
struct Sha256 {
    uint32_t h[8] = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u };
    static uint32_t ror( uint32_t x, int n ) { return ( x >> n ) | ( x << ( 32 - n ) ); }
    void block( const uint8_t *p ) {
        static const uint32_t k[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
        uint32_t w[64];
        for( int i = 0; i < 16; i++ )
            w[i] = ( (uint32_t) p[i*4] << 24 ) | ( (uint32_t) p[i*4+1] << 16 )
                 | ( (uint32_t) p[i*4+2] << 8 ) | (uint32_t) p[i*4+3];
        for( int i = 16; i < 64; i++ ) {
            const uint32_t s0 = ror( w[i-15], 7 ) ^ ror( w[i-15], 18 ) ^ ( w[i-15] >> 3 );
            const uint32_t s1 = ror( w[i-2], 17 ) ^ ror( w[i-2], 19 ) ^ ( w[i-2] >> 10 );
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for( int i = 0; i < 64; i++ ) {
            const uint32_t S1 = ror(e,6) ^ ror(e,11) ^ ror(e,25);
            const uint32_t ch = ( e & f ) ^ ( ~e & g );
            const uint32_t t1 = hh + S1 + ch + k[i] + w[i];
            const uint32_t S0 = ror(a,2) ^ ror(a,13) ^ ror(a,22);
            const uint32_t mj = ( a & b ) ^ ( a & c ) ^ ( b & c );
            const uint32_t t2 = S0 + mj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    std::string of( const std::vector<uint8_t> &data ) {
        std::vector<uint8_t> m( data );
        const uint64_t bits = (uint64_t) data.size() * 8;
        m.push_back( 0x80 );
        while( m.size() % 64 != 56 ) m.push_back( 0 );
        for( int i = 7; i >= 0; i-- ) m.push_back( (uint8_t) ( bits >> ( i * 8 ) ) );
        for( size_t i = 0; i < m.size(); i += 64 ) block( m.data() + i );
        char out[65];
        for( int i = 0; i < 8; i++ ) snprintf( out + i * 8, 9, "%08x", h[i] );
        return std::string( out, 64 );
    }
};

const double kPi = 3.14159265358979323846;

// REPLICA of app/model/sfeelflowskeleton.h's sfeelflowskel:: constants --
// quoted here so section F's force figures describe what is actually DRAWN,
// not a guess. The probe is ENGINE-side (it links tw_sidecar and no Qt) and
// check_layering rule 1 forbids a tw303a file including an app/ header, so it
// cannot share the real ones. Update in the same commit that changes them.
const double kSwayDeg    = 20.0;
const double kNodDeg     = 10.0;
const double kBounceFrac = 0.06;

// ---- a minimal 16-bit WAV reader (the fixtures are all 16-bit) -------------
std::vector<float> readWav16( const char *path, uint32_t *outRate )
{
    std::vector<float> out;
    FILE *f = fopen( path, "rb" );
    if( !f ) return out;
    char riff[12];
    if( fread( riff, 1, 12, f ) != 12 ) { fclose( f ); return out; }
    uint16_t nch = 1, bits = 16;
    uint32_t sr = 48000;
    bool haveFmt = false;
    std::vector<int16_t> pcm;
    for( ;; ) {
        char id[4];
        uint32_t sz;
        if( fread( id, 1, 4, f ) != 4 ) break;
        if( fread( &sz, 4, 1, f ) != 1 ) break;
        if( !memcmp( id, "fmt ", 4 ) ) {
            std::vector<uint8_t> b( sz );
            if( fread( b.data(), 1, sz, f ) != sz ) break;
            if( sz >= 16 ) {
                memcpy( &nch, b.data() + 2, 2 );
                memcpy( &sr, b.data() + 4, 4 );
                memcpy( &bits, b.data() + 14, 2 );
                haveFmt = true;
            }
        } else if( !memcmp( id, "data", 4 ) ) {
            pcm.resize( sz / 2 );
            if( fread( pcm.data(), 1, sz, f ) != sz ) break;
        } else {
            fseek( f, (long) sz + ( sz & 1 ), SEEK_CUR );
        }
    }
    fclose( f );
    if( !haveFmt || bits != 16 || nch == 0 || pcm.empty() ) return out;
    if( outRate ) *outRate = sr;
    const size_t n = pcm.size() / nch;
    out.resize( n );
    for( size_t i = 0; i < n; i++ ) {
        double a = 0.0;
        for( int c = 0; c < nch; c++ ) a += pcm[i * nch + c] / 32768.0;
        out[i] = (float) ( a / nch );
    }
    return out;
}

// ---- statistics ------------------------------------------------------------
double mean( const std::vector<double> &v )
{ double s = 0; for( double x : v ) s += x; return v.empty() ? 0 : s / v.size(); }

double rms( const std::vector<double> &v )
{ double s = 0; for( double x : v ) s += x * x; return v.empty() ? 0 : std::sqrt( s / v.size() ); }

double maxabs( const std::vector<double> &v )
{ double m = 0; for( double x : v ) m = std::max( m, std::fabs( x ) ); return m; }

double corr( const std::vector<double> &a, const std::vector<double> &b )
{
    const size_t n = std::min( a.size(), b.size() );
    if( n < 2 ) return 0.0;
    double ma = 0, mb = 0;
    for( size_t i = 0; i < n; i++ ) { ma += a[i]; mb += b[i]; }
    ma /= n; mb /= n;
    double sab = 0, sa = 0, sb = 0;
    for( size_t i = 0; i < n; i++ ) {
        const double da = a[i] - ma, db = b[i] - mb;
        sab += da * db; sa += da * da; sb += db * db;
    }
    return ( sa > 0 && sb > 0 ) ? sab / std::sqrt( sa * sb ) : 0.0;
}

double skewness( const std::vector<double> &v )
{
    const double m = mean( v );
    double s2 = 0, s3 = 0;
    for( double x : v ) { const double d = x - m; s2 += d * d; s3 += d * d * d; }
    if( v.empty() || s2 <= 0 ) return 0.0;
    s2 /= v.size(); s3 /= v.size();
    return s3 / std::pow( s2, 1.5 );
}

// 1.5 for a pure sinusoid, 3.0 for a Gaussian, >> 3 for a spiky impulsive signal.
double kurtosis( const std::vector<double> &v )
{
    const double m = mean( v );
    double s2 = 0, s4 = 0;
    for( double x : v ) { const double d = x - m; s2 += d * d; s4 += d * d * d * d; }
    if( v.empty() || s2 <= 0 ) return 0.0;
    s2 /= v.size(); s4 /= v.size();
    return s4 / ( s2 * s2 );
}

std::vector<double> deriv( const std::vector<double> &v, double dt )
{
    std::vector<double> d( v.size(), 0.0 );
    for( size_t i = 1; i + 1 < v.size(); i++ ) d[i] = ( v[i + 1] - v[i - 1] ) / ( 2 * dt );
    if( v.size() > 1 ) { d[0] = d[1]; d[v.size() - 1] = d[v.size() - 2]; }
    return d;
}

// Fraction of the time the speed sits under `frac` of its own peak -- the DWELL.
// A pure sinusoid's closed form is (2/pi)*asin(frac); a body that hangs at the
// top of a movement and then drops reads much higher.
double dwellFraction( const std::vector<double> &vel, double frac )
{
    const double m = maxabs( vel );
    if( m <= 0 ) return 1.0;
    size_t n = 0;
    for( double x : vel ) if( std::fabs( x ) < frac * m ) n++;
    return (double) n / (double) vel.size();
}

} // namespace

int main( int argc, char **argv )
{
    if( argc < 2 ) {
        printf( "usage: body_probe <fixture.wav> [breakStartSec breakEndSec] [trace.csv]\n" );
        return 2;
    }
    const char  *path    = argv[1];
    const double breakS  = argc > 2 ? atof( argv[2] ) : -1.0;
    const double breakE  = argc > 3 ? atof( argv[3] ) : -1.0;
    const char  *csvPath = argc > 4 ? argv[4] : nullptr;

    // --hash-payloads may appear in any argument position.
    bool hashOnly = false;
    for( int i = 2; i < argc; i++ )
        if( !strcmp( argv[i], "--hash-payloads" ) ) hashOnly = true;
    bool assertCrossover = false;
    for( int i = 2; i < argc; i++ )
        if( !strcmp( argv[i], "--assert-crossover" ) ) assertCrossover = true;
    // Proposal 44 C4b/C5: report the per-unit DRIVE MAGNITUDE, which is the
    // candidate for `urgeNorm`'s source. Measurement first, then the constant
    // -- the same order proposal 44 has used for every number in it.
    bool urgeMode = false;
    for( int i = 2; i < argc; i++ )
        if( !strcmp( argv[i], "--urge" ) ) urgeMode = true;

    uint32_t rate = 0;
    std::vector<float> sig = readWav16( path, &rate );
    if( sig.empty() ) { printf( "body_probe: cannot read %s (16-bit WAV only)\n", path ); return 1; }
    const float *chans[1] = { sig.data() };

    twGrooveFrontEndParams fp;
    fp.nBands = 64; fp.fMinHz = 40.0f; fp.fMaxHz = 16000.0f; fp.envRateHz = 200.0f;
    fp.nRegions = 10; fp.medianHalfWidthSec = 0.5f; fp.thresholdFactor = 1.5f;
    fp.energyFloorFraction = 0.05f; fp.minSeparationSec = 0.03f;

    twGrooveField field = twGrooveAnalyzeFrontEnd( chans, 1, sig.size(), rate, fp );

    twGrooveAnalysisParams ap;
    twGrooveAspectPayloads pay =
        twGrooveBuildAspectPayloads( chans, 1, (uint64_t) sig.size(), rate, ap );
    if( pay.nUnits == 0 ) { printf( "body_probe: no analysis (no recoverable tatum)\n" ); return 1; }

    // AC-INV's check: exactly three lines, the SHA-256 of each payload and
    // NOTHING else -- no fixture path, no geometry, nothing that could drift
    // when the probe's human-readable sections are edited. That separation is
    // the point: AC0.4 and AC1.2 both REQUIRE those sections to change, and a
    // hash over the whole stdout would then go red on a green milestone.
    if( hashOnly ) {
        Sha256 a, b, c;
        printf( "groove.res %s\n", a.of( pay.resPayload ).c_str() );
        printf( "groove.ev  %s\n", b.of( pay.evPayload ).c_str() );
        printf( "groove.dyn %s\n", c.of( pay.dynPayload ).c_str() );
        return 0;
    }

    const std::vector<twGrooveResRecord> res =
        twGrooveDecodeResPayload( pay.resPayload.data(), pay.resPayload.size(), pay.nUnits );
    const std::vector<twGrooveDynRecord> dyn =
        twGrooveDecodeDynPayload( pay.dynPayload.data(), pay.dynPayload.size(), pay.nUnits );
    const twGroovePendulumResult pr = twGroovePendulumAnalyze( field, ap.pendulum );

    std::vector<std::string> names;
    for( const auto &t : pr.unitTrajectories ) names.push_back( t.name );

    const size_t nHops = std::min( res.size(), dyn.size() );
    const double dt    = (double) pay.hopFrames / (double) rate;

    printf( "fixture: %s  (%zu frames @ %u Hz = %.2f s)\n",
            path, sig.size(), rate, sig.size() / (double) rate );
    printf( "hops=%zu  dt=%.4f s  units=%u  recovered tatum=%.4f s\n",
            nHops, dt, pay.nUnits, pr.tatumPeriodSec );
    printf( "seeded ladder:" );
    for( size_t u = 0; u < names.size(); u++ )
        printf( " %s=%.3fHz", names[u].c_str(),
                pr.unitTrajectories[u].omega0 > 0 ? pr.unitTrajectories[u].omega0 / ( 2 * kPi ) : 0.0 );
    printf( "\n\n" );

    // ---- the EXACT sFeelFlowPoseAt mapping, BY NAME --------------------------
    // REPLICA of kPartUnitName in main/objects/track/src/sfeelflowpose.cpp.
    // Goes stale the moment C1 re-points the head; update in the same commit.
    // REPLICA of kPartUnitName in main/objects/track/src/sfeelflowpose.cpp.
    // "" for the head since C1: it is DERIVED from the trunk (a one-pole lag),
    // not mapped to a unit. The probe reproduces that derivation below so its
    // headNod column keeps meaning what the puppet draws.
    const char *partUnit[5] = { "bounce", "sway", "limbs", "", "twobar" };
    const char *partName[5] = { "bounceY(pelvis)", "sway(torso)", "armSwing",
                                "headNod", "hipShift" };
    std::vector<std::vector<double>> disp( 5 ), ener( 5 );
    double partHz[5] = { 0, 0, 0, 0, 0 };
    for( int p = 0; p < 5; p++ ) {
        size_t unit = names.size();
        for( size_t u = 0; u < names.size(); u++ )
            if( names[u] == partUnit[p] ) { unit = u; break; }
        disp[p].assign( nHops, 0.0 );
        ener[p].assign( nHops, 0.0 );
        if( unit >= names.size() ) continue;
        if( pr.unitTrajectories[unit].omega0 > 0 )
            partHz[p] = pr.unitTrajectories[unit].omega0 / ( 2 * kPi );
        for( size_t h = 0; h < nHops; h++ ) {
            if( unit >= res[h].unitPower.size() || unit >= dyn[h].units.size() ) continue;
            const double power = res[h].unitPower[unit];
            const double e     = power > 0 ? std::sqrt( power ) : 0.0;
            ener[p][h] = e;
            disp[p][h] = std::max( -1.0, std::min( 1.0, e * dyn[h].units[unit].cosMet ) );
        }
    }

    // The head, DERIVED from the trunk exactly as sFeelFlowPoseAt does
    // (proposal 44 C1 option D). REPLICA of deriveHeadNod().
    {
        const double tau = 0.126, gain = 0.5, ratio = 20.0 / 10.0;
        const double alpha = dt / ( tau + dt );
        double lag = nHops ? disp[1][0] : 0.0;
        for( size_t h = 0; h < nHops; h++ ) {
            lag += alpha * ( disp[1][h] - lag );
            disp[3][h] = std::max( -1.0, std::min( 1.0,
                             gain * ( lag - disp[1][h] ) * ratio ) );
            ener[3][h] = ener[1][h];
        }
        partHz[3] = partHz[1];

        // WHAT THE PUPPET ACTUALLY DRAWS, in degrees, for the head RELATIVE to
        // the trunk -- which is the only thing an eye can see as "the head
        // moving". Reported next to the trunk's own lean so the two are
        // comparable, because the head's WORLD angle is dominated by the trunk
        // carrying it and looks perfectly healthy while the relative nod is
        // invisible.
        double nodSq = 0.0, nodPk = 0.0, swaySq = 0.0, swayPk = 0.0;
        for( size_t h = 0; h < nHops; h++ ) {
            const double nod  = disp[3][h] * kNodDeg;
            const double sway = disp[1][h] * kSwayDeg;
            nodSq  += nod * nod;   nodPk  = std::max( nodPk,  std::fabs( nod ) );
            swaySq += sway * sway; swayPk = std::max( swayPk, std::fabs( sway ) );
        }
        if( nHops ) {
            printf( "DRAWN HEAD NOD (relative to the trunk, which is what an eye"
                    " can see):\n" );
            printf( "  nod  rms %6.3f deg  peak %6.3f deg\n",
                    std::sqrt( nodSq / (double) nHops ), nodPk );
            printf( "  sway rms %6.3f deg  peak %6.3f deg   -> the head nods"
                    " %.1f%% as many degrees as the trunk leans\n\n",
                    std::sqrt( swaySq / (double) nHops ), swayPk,
                    swaySq > 0.0 ? 100.0 * std::sqrt( nodSq / swaySq ) : 0.0 );
        }
    }

    // --urge: WHAT DOES THE MUSIC ACTUALLY ASK FOR? -------------------------
    //
    // `urgeNorm` needs a source, and the candidates are not equal. The one
    // this mode measures is
    //
    //     drive = hypot( support, tension )   ==   |k * F_p(t)|
    //
    // because `support` and `tension` are k*F*cos(phi) and k*F*sin(phi), so
    // their MAGNITUDE is the drive with the resonator's own phase -- and
    // therefore its response -- divided straight out. That is the property
    // that makes it a candidate at all: `perUnitPower` and `dissip` are both
    // the body's RESPONSE, so using either as "what the music asks for" makes
    // urge and achieved the same quantity and match trivially 1. Circular.
    //
    // Reported as PERCENTILES over the whole fixture rather than as a mean,
    // because what a reference constant has to be calibrated against is the
    // LOUD part of a track, not its average.
    if( urgeMode ) {
        std::vector<twGrooveDynRecord> dyn = twGrooveDecodeDynPayload(
            pay.dynPayload.data(), (uint64_t) pay.dynPayload.size(), pay.nUnits );
        if( dyn.empty() ) { printf( "body_probe: no dyn payload\n" ); return 1; }
        printf( "urge probe: %s  %u units, %zu hops\n", path, pay.nUnits, dyn.size() );
        printf( "  unit          k     rawP99   F=drive/k p99   SMOOTHED F p99"
                " (tau 1 s)\n" );
        for( uint32_t u = 0; u < pay.nUnits; u++ ) {
            std::vector<double> d; d.reserve( dyn.size() );
            // The same one-pole the read side would apply: urge is a LEVEL,
            // not an onset train. Between onsets the raw flux is ~0, so a
            // per-hop urge would be zero most of the time and the window mean
            // would be meaningless.
            const double kk = ( u < pay.counterTension.size()
                                && pay.counterTension[u].k > 0.0 )
                              ? pay.counterTension[u].k : 1.0;
            const double dt = 0.01, tau = 1.0;
            const double a  = dt / ( tau + dt );
            double sm = 0.0;
            std::vector<double> smv; smv.reserve( dyn.size() );
            for( const twGrooveDynRecord &r : dyn ) {
                if( u >= r.units.size() ) continue;
                // DIVIDE THE COUPLING OUT. hypot(support,tension) is |k*F|;
                // k is OURS, F is the music. Without the divide, twobar reads
                // twice everything else purely because its k is 3.5 to their
                // 1.5 -- a property of the model presented as a property of
                // the material.
                const double v = std::hypot( (double) r.units[u].support,
                                             (double) r.units[u].tension ) / kk;
                d.push_back( v );
                sm += a * ( v - sm );
                smv.push_back( sm );
            }
            if( d.empty() ) continue;
            std::sort( d.begin(), d.end() );
            std::sort( smv.begin(), smv.end() );
            auto pct = []( const std::vector<double> &v, double q ) {
                return v[ (size_t) std::min( (double) v.size() - 1.0,
                                             q * (double) v.size() ) ];
            };
            const char *nm = u < pay.counterTension.size()
                           ? pay.counterTension[u].name.c_str() : "?";
            printf( "  %-10s %4.1f  %9.5f     %9.5f        %9.5f\n",
                    nm, kk, pct( d, 0.99 ) * kk, pct( d, 0.99 ),
                    pct( smv, 0.99 ) );
        }
        return 0;
    }

    // --assert-crossover: is a rotational DOF being FLUNG or CARRIED?
    // Uses the ZERO-CROSSING rate of what is actually DRAWN, never the unit's
    // seeded omega0. Measured while building C1: every part's displacement
    // crosses zero ~4 times a second regardless of its seed, because arg(z)
    // JITTERS with the drive even while its mean drift stays near omega. The
    // seed describes the mean drift; the body would have to execute the
    // jitter, so the jitter is what a torque demand must be computed from.
    if( assertCrossover ) {
        if( std::fabs( pr.tatumPeriodSec - 0.25 ) > 0.02 ) {
            printf( "body_probe: REFUSING -- recovered tatum %.4f s is not the "
                    "0.25 s regime; the ladder is mis-seeded and the kinematics "
                    "measure that, not the model.\n", pr.tatumPeriodSec );
            return 2;
        }
        // f_cross now comes from tw_body (proposal 44 C2), not from two
        // constants copied out of a document. AC2.3's whole point: the closed
        // form and the amplitude live in ONE place, and the probe reads it.
        const twBodyMeasures body;                       // 75 kg / 1.75 m
        const double d10 = 10.0 * kPi / 180.0, d20 = 20.0 * kPi / 180.0;
        struct Row { const char *what; int part; double fCross; };
        // Only the two ROTATIONAL DOFs: bounce is a force ratio in g under its
        // own bound, and hipShift/armSwing are translations and an abduction
        // with no defined tau_gravity here. The amplitudes are the puppet's own
        // display constants, and the crossover DEPENDS on them.
        const Row rows[] = {
            { "head",  3, twBodyCrossoverHz( body.segment( twBodySeg::HeadNeck ), d10 ) },
            { "trunk", 1, twBodyCrossoverHz( body.segment( twBodySeg::Trunk ),    d20 ) },
        };
        int bad = 0;
        // THE HEAD IS MEASURED IN WORLD SPACE, not in the trunk's frame.
        // f_cross asks what a segment's own motion costs, and the head's motion
        // is the trunk's PLUS its small relative offset -- since C1 the pose
        // carries only the offset. Measuring the offset alone reports a
        // near-zero signal's noise as a frequency, which is exactly the trap
        // this comment exists to stop the next reader falling into.
        // Both series in DEGREES of world angle, so the rms column compares.
        std::vector<double> headWorld( nHops, 0.0 ), trunkWorld( nHops, 0.0 );
        for( size_t h = 0; h < nHops; h++ ) {
            trunkWorld[h] = disp[1][h] * kSwayDeg;
            headWorld[h]  = disp[1][h] * kSwayDeg + disp[3][h] * kNodDeg;
        }
        for( const Row &r : rows ) {
            const std::vector<double> &sig = ( r.part == 3 ) ? headWorld : trunkWorld;
            size_t zc = 0;
            for( size_t h = 1; h < nHops; h++ )
                if( sig[h - 1] * sig[h] < 0.0 ) zc++;
            const double dur = (double) nHops * dt;
            const double f   = dur > 0 ? (double) zc / 2.0 / dur : 0.0;
            const double ratio = ( f / r.fCross ) * ( f / r.fCross );
            double s2 = 0; for( double v : sig ) s2 += v * v;
            printf( "%-6s drawn %.3f Hz  f_cross %.3f Hz  ratio %.2f  rms %.3f deg  %s\n",
                    r.what, f, r.fCross, ratio,
                    nHops ? std::sqrt( s2 / (double) nHops ) : 0.0,
                    ratio > 3.0 ? "FLUNG (over 3.0)" : "ok" );
            if( ratio > 3.0 ) bad++;
        }
        return bad ? 1 : 0;
    }

    if( csvPath ) {
        FILE *f = fopen( csvPath, "w" );
        if( f ) {
            fprintf( f, "t,bounceY,sway,armSwing,headNod,hipShift,eB,eS,eL,eR,eT\n" );
            for( size_t h = 0; h < nHops; h++ ) {
                fprintf( f, "%.3f", h * dt );
                for( int p = 0; p < 5; p++ ) fprintf( f, ",%.6f", disp[p][h] );
                for( int p = 0; p < 5; p++ ) fprintf( f, ",%.6f", ener[p][h] );
                fprintf( f, "\n" );
            }
            fclose( f );
            printf( "trace: %s\n\n", csvPath );
        }
    }

    const double sinDwell = 2.0 / kPi * std::asin( 0.10 );

    // ---- A. whole-run kinematics --------------------------------------------
    printf( "== A. per-part kinematics, WHOLE RUN ==\n" );
    printf( "%-16s %7s %8s %8s %9s %9s %9s %9s %9s\n", "part", "f(Hz)", "rms",
            "peak", "dwell10%", "crestAcc", "skew(v)", "skew(a)", "kurt(a)" );
    for( int p = 0; p < 5; p++ ) {
        const std::vector<double> v = deriv( disp[p], dt );
        const std::vector<double> a = deriv( v, dt );
        printf( "%-16s %7.3f %8.4f %8.4f %9.3f %9.3f %9.3f %9.3f %9.3f\n",
                partName[p], partHz[p], rms( disp[p] ), maxabs( disp[p] ),
                dwellFraction( v, 0.10 ), rms( a ) > 0 ? maxabs( a ) / rms( a ) : 0.0,
                skewness( v ), skewness( a ), kurtosis( a ) );
    }
    printf( "  PURE SINUSOID: dwell10%%=%.4f crestAcc=1.4142 skew=0 kurt(a)=1.5000\n"
            "  A BODY WITH WEIGHT: dwell HIGH (it hangs at the top of a movement),\n"
            "  crestAcc HIGH (a short hard stance), skew(a) STRONGLY nonzero (gravity\n"
            "  acts one way only), kurt(a) >> 1.5 (rare impulses among long coasts).\n\n",
            sinDwell );

    // ---- A2. the shape of ONE movement cycle --------------------------------
    printf( "== A2. waveform SHAPE on the longest steady, high-energy stretch ==\n" );
    printf( "%-16s %8s %9s %9s %9s %9s %9s\n", "part", "hops", "dwell10%",
            "crestAcc", "skew(v)", "skew(a)", "kurt(a)" );
    for( int p = 0; p < 5; p++ ) {
        double pk = 0;
        for( double e : ener[p] ) pk = std::max( pk, e );
        if( pk <= 0 ) { printf( "%-16s %8s\n", partName[p], "-" ); continue; }
        size_t bestA = 0, bestN = 0, curA = 0, curN = 0;
        for( size_t h = 0; h < nHops; h++ ) {
            if( ener[p][h] > 0.60 * pk ) { if( curN == 0 ) curA = h; curN++; }
            else { if( curN > bestN ) { bestN = curN; bestA = curA; } curN = 0; }
        }
        if( curN > bestN ) { bestN = curN; bestA = curA; }
        if( bestN < 100 ) {
            printf( "%-16s %8zu   (never steady for 1 s -- its ENVELOPE is the motion)\n",
                    partName[p], bestN );
            continue;
        }
        const std::vector<double> x( disp[p].begin() + bestA, disp[p].begin() + bestA + bestN );
        const std::vector<double> v = deriv( x, dt );
        const std::vector<double> a = deriv( v, dt );
        printf( "%-16s %8zu %9.3f %9.3f %9.3f %9.3f %9.3f\n", partName[p], bestN,
                dwellFraction( v, 0.10 ), rms( a ) > 0 ? maxabs( a ) / rms( a ) : 0.0,
                skewness( v ), skewness( a ), kurtosis( a ) );
    }
    printf( "  Same closed forms: dwell10%%=%.4f crestAcc=1.4142 skew=0 kurt(a)=1.5000\n\n",
            sinDwell );

    // ---- B. head vs torso ----------------------------------------------------
    printf( "== B. head / torso subordination ==\n" );
    const std::vector<double> &torso = disp[1];
    const std::vector<double> &head  = disp[3];
    printf( "frequency ratio head:torso        = %.3f : 1\n",
            partHz[1] > 0 ? partHz[3] / partHz[1] : 0.0 );
    printf( "corr(headNod, sway), zero lag     = %+.4f\n", corr( head, torso ) );
    double bestC = 0; int bestL = 0;
    for( int L = -100; L <= 100; L++ ) {
        std::vector<double> a, b;
        for( size_t i = 0; i < nHops; i++ ) {
            const long j = (long) i + L;
            if( j < 0 || j >= (long) nHops ) continue;
            a.push_back( head[i] );
            b.push_back( torso[(size_t) j] );
        }
        const double c = corr( a, b );
        if( std::fabs( c ) > std::fabs( bestC ) ) { bestC = c; bestL = L; }
    }
    printf( "best |corr| over lag +-1000 ms    = %+.4f at lag %+d ms\n", bestC, bestL * 10 );
    printf( "  A neck is a CONSTRAINT, not a correlation: whatever the drive, a real\n"
            "  head's world angle is the torso's PLUS a small, LAGGING neck angle. The\n"
            "  numbers above are what two independent oscillators produce.\n\n" );

    // ---- C. cross-part coupling ---------------------------------------------
    printf( "== C. cross-part coupling matrix, corr of displacement ==\n" );
    printf( "%-16s", "" );
    for( int q = 0; q < 5; q++ ) printf( "%12.12s", partName[q] );
    printf( "\n" );
    for( int p = 0; p < 5; p++ ) {
        printf( "%-16s", partName[p] );
        for( int q = 0; q < 5; q++ ) printf( "%12.3f", corr( disp[p], disp[q] ) );
        printf( "\n" );
    }
    printf( "  The ensemble integrates each unit in its OWN loop with no reference to\n"
            "  any other unit's state (twgroovependulum.cc runPass1), so every entry\n"
            "  off the diagonal is SHARED DRIVE, never a force one part exerts on\n"
            "  another. There is no momentum to conserve because there is no mass.\n\n" );

    // ---- D. the sudden stop --------------------------------------------------
    if( breakS >= 0 && breakE > breakS ) {
        const long bIn = (long) ( breakS / dt ), bOut = (long) ( breakE / dt );
        printf( "== D. the sudden stop: SILENCE over [%.2f, %.2f] s ==\n", breakS, breakE );
        printf( "%-16s %9s %11s %11s %9s %12s\n", "part", "rms pre", "rms 1sthalf",
                "rms 2ndhalf", "rms post", "1sthalf/pre" );
        for( int p = 0; p < 5; p++ ) {
            auto win = [&]( long a, long b ) {
                std::vector<double> w;
                for( long i = std::max( 0L, a ); i < std::min( (long) nHops, b ); i++ )
                    w.push_back( disp[p][(size_t) i] );
                return w;
            };
            const long   mid  = ( bIn + bOut ) / 2;
            const double pre  = rms( win( bIn - 200, bIn ) );
            const double h1   = rms( win( bIn, mid ) );
            const double h2   = rms( win( mid, bOut ) );
            const double post = rms( win( bOut, bOut + 200 ) );
            printf( "%-16s %9.4f %11.4f %11.4f %9.4f %11.1f%%\n",
                    partName[p], pre, h1, h2, post, pre > 0 ? 100.0 * h1 / pre : 0.0 );
        }
        printf( "  A real body ARRESTS itself in about one movement cycle -- that arrest\n"
                "  is muscular work, and it is the single most legible thing a dancer\n"
                "  does at a break. A linear damped resonator can only ring DOWN, over\n"
                "  its own dampingCycles (4-8 periods), whatever the material does.\n\n" );
    }

    // ---- E. gravity signature on the vertical -------------------------------
    printf( "== E. gravity signature on the pelvis ==\n" );
    {
        const std::vector<double> v = deriv( disp[0], dt );
        size_t up = 0, down = 0;
        for( double s : v ) { if( s > 0 ) up++; else if( s < 0 ) down++; }
        const std::vector<double> a = deriv( v, dt );
        double aUp = 0, aDown = 0; size_t nu = 0, nd = 0;
        for( double x : a ) { if( x > 0 ) { aUp += x; nu++; } else { aDown += -x; nd++; } }
        printf( "time rising = %.1f%%   time falling = %.1f%%\n",
                100.0 * up / (double) ( up + down ), 100.0 * down / (double) ( up + down ) );
        printf( "mean |accel| up = %.3f   down = %.3f   ratio = %.3f\n",
                nu ? aUp / nu : 0.0, nd ? aDown / nd : 0.0,
                ( nu && nd ) ? ( aUp / nu ) / ( aDown / nd ) : 0.0 );
        printf( "  A bouncing body is BALLISTIC going up and down (one constant -g) and\n"
                "  pays for it in a short, high-force stance. Its vertical acceleration\n"
                "  is therefore strongly one-sided. A sinusoid's is exactly symmetric.\n\n" );
    }

    // ---- F. the forces the DRAWN motion would demand ------------------------
    // The one question proposal 44 says the present model cannot answer. Segment
    // values are de Leva 1996 / Winter as quoted in proposal 44 section 3 and
    // carry that section's own VERIFY flag: these are order-of-magnitude demands
    // for a 75 kg / 1.75 m body, not validated biomechanics.
    {
        const double M = 75.0, H = 1.75, g = 9.81;
        const double mHead = 0.069 * M, dHead = 0.5 * 0.130 * H, rHead = 0.30 * 0.130 * H;
        const double IHead = mHead * ( rHead * rHead + dHead * dHead );
        const double mTrunk = 0.435 * M, dTrunk = 0.45 * 0.288 * H, rTrunk = 0.37 * 0.288 * H;
        const double ITrunk = mTrunk * ( rTrunk * rTrunk + dTrunk * dTrunk );
        const double swayAmp = kSwayDeg * kPi / 180.0, nodAmp = kNodDeg * kPi / 180.0;
        const double bounceAmpM = kBounceFrac * H;

        printf( "== F. what the DRAWN motion would demand of a 75 kg / 1.75 m body ==\n" );
        printf( "   (VERIFY-flagged segment values, proposal 44 section 3;\n"
                "    amplitudes are the puppet's OWN display constants)\n" );
        printf( "%-24s %7s %9s %11s %13s %13s %8s %10s\n", "DOF", "f(Hz)", "amp(deg)",
                "I(kg m^2)", "tau_inertial", "tau_gravity", "ratio", "f_cross" );
        struct Row { const char *what; double I, amp, hz, mgd; };
        const Row rows[] = {
            { "head nod   (reference)", IHead,  nodAmp,  partHz[3], mHead  * g * dHead  },
            { "torso lean (sway)",      ITrunk, swayAmp, partHz[1], mTrunk * g * dTrunk },
        };
        for( const Row &r : rows ) {
            const double w  = 2 * kPi * r.hz;
            const double ti = r.I * r.amp * w * w;
            const double tg = r.mgd * std::sin( r.amp );
            // The frequency at which this DOF's INERTIAL demand equals its own
            // gravity moment: I*amp*w^2 = m*g*d*sin(amp)  =>  a closed form, and
            // the single most actionable number here -- it says which metrical
            // level a body part may physically occupy. Above it the part is
            // being flung; below it, carried.
            const double fx = ( r.I * r.amp > 0 )
                                ? std::sqrt( r.mgd * std::sin( r.amp ) / ( r.I * r.amp ) ) / ( 2 * kPi )
                                : 0.0;
            printf( "%-24s %7.3f %9.1f %11.4f %10.2f Nm %10.2f Nm %8.2f %8.3fHz\n",
                    r.what, r.hz, r.amp * 180 / kPi, r.I, ti, tg, tg > 0 ? ti / tg : 0.0, fx );
        }
        const double wB = 2 * kPi * partHz[0];
        printf( "%-24s %7.3f %9.3f %11s %10.2f N  %10.2f N  %8.2f %10s\n",
                "pelvis bounce (bounce)", partHz[0], bounceAmpM, "(m amp)",
                M * bounceAmpM * wB * wB, M * g,
                ( M * g ) > 0 ? ( bounceAmpM * wB * wB ) / g : 0.0,
                "1g@1.54Hz" );
        printf( "   peak vertical acceleration = %.2f g\n", bounceAmpM * wB * wB / g );
        printf( "  f_cross is where I*amp*w^2 == m*g*d*sin(amp): BELOW it a part is carried by\n"
                "  muscles working against its own weight, ABOVE it the motion is inertial\n"
                "  and the weight is beside the point. A DOF driven far above its own\n"
                "  f_cross is being flung,\n"
                "  not carried -- and inertia goes as omega^2, so halving a part's\n"
                "  frequency quarters what it costs. That factor is the whole argument\n"
                "  for asking which METRICAL LEVEL each body part belongs to.\n" );
    }

    // ---- G. the puppet's own kinematic chain, replicated exactly -------------
    // REPLICA of app/model/sfeelflowskeleton.cpp's sFeelFlowSkeletonFor, for a
    // 200x400 dock. Not a property of the analysis at all -- a property of the
    // GEOMETRY, and the direct cause of "the head is not subordinate to the
    // torso". This is a replica and can only ever be one: the probe is
    // engine-side and cannot include an app/ header (check_layering rule 1),
    // so the REAL gate on this is qxa.feel_flow_puppet_chain, which drives
    // the production function through assert-puppet-skeleton. Keep in step.
    {
        struct Pt { double x, y; };
        auto rot = []( Pt pivot, Pt p, double deg ) {
            const double r = deg * kPi / 180.0, c = std::cos( r ), s = std::sin( r );
            const double dx = p.x - pivot.x, dy = p.y - pivot.y;
            return Pt{ pivot.x + dx * c - dy * s, pivot.y + dx * s + dy * c };
        };
        // lean from world-up, degrees, positive = clockwise on screen
        auto lean = []( Pt a, Pt b ) { return std::atan2( b.x - a.x, a.y - b.y ) * 180.0 / kPi; };

        const double L = 6, T = 6, R = 194, B = 394;
        const double bw = R - L, bh = B - T, cx = ( L + R ) / 2;
        const double groundY = B - bh * 0.04;
        const double legLen = bh * 0.30, torsoLen = bh * 0.30, headR = bh * 0.070;
        const double shoulderW = bw * 0.12;

        printf( "\n== G. the puppet's kinematic chain (paintEvent geometry, replicated) ==\n" );
        printf( "%8s %12s %16s %14s\n", "sway", "torso lean", "neck->head lean", "shoulder bar" );
        const double swayVals[] = { 0.0, 0.5, 1.0, -1.0 };
        for( double sway : swayVals ) {
            const Pt pelvis{ cx, groundY - legLen };
            const Pt neck  = rot( pelvis, { pelvis.x, pelvis.y - torsoLen }, sway * kSwayDeg );
            const Pt headC = rot( neck, { neck.x, neck.y - headR * 1.35 }, 0.0 );
            const Pt headBase{ headC.x, headC.y + headR };
            const Pt shL{ neck.x - shoulderW, neck.y + torsoLen * 0.06 };
            const Pt shR{ neck.x + shoulderW, neck.y + torsoLen * 0.06 };
            printf( "%8.2f %11.3f%s %15.3f%s %13.3f%s\n", sway,
                    lean( pelvis, neck ), "\u00b0", lean( neck, headBase ), "\u00b0",
                    std::atan2( shR.y - shL.y, shR.x - shL.x ) * 180.0 / kPi, "\u00b0" );
        }
        printf( "  Every segment ABOVE the neck is built at (neck.x, neck.y - r) -- straight\n"
                "  up in the WORLD, not along the torso axis -- so the head, the shoulder\n"
                "  bar and both arms inherit the neck's TRANSLATION and none of its\n"
                "  ROTATION. A torso at 20 deg with a head and shoulders at 0.000 deg is a\n"
                "  figure with no spine, and it is fixable without any of proposal 44:\n"
                "  build the segments in TORSO-LOCAL coordinates.\n" );
    }
    return 0;
}
