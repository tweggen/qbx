// test_twwarpmap.cpp — twWarpMap exactness properties (proposal 28 W1).
//
// House pattern: plain main(), CHECK counting failures, returns count.

#include "tw/core/twwarpmap.h"

#include <iostream>
#include <vector>

static int g_fails = 0;
#define CHECK( cond, msg )                                                    \
    do {                                                                      \
        if( !( cond ) ) {                                                     \
            std::cerr << "FAIL: " << ( msg ) << "  [" << __LINE__ << "]\n";   \
            ++g_fails;                                                        \
        }                                                                     \
    } while( 0 )

int main()
{
    std::cout << "twwarpmap_test starting\n";

    // --- No anchors: bit-identical to the historic scalar expressions ----
    {
        const Fraction stretch( 125216, 103939 );   // a real project ratio
        twWarpMap m( stretch );
        const int64_t probes[] = { 0, 1, 103939, 125216, 480000, 7 };
        for( int64_t p : probes ) {
            CHECK( m.srcToWarped( Fraction( p ) ) == Fraction( p ) * stretch,
                   "no-anchor srcToWarped == pos*stretch (exact)" );
            CHECK( m.srcToWarpedFloor( p )
                       == ( Fraction( p ) * stretch ).floorToInt(),
                   "no-anchor floor == historic render-boundary expression" );
        }
        // Exact inverse.
        for( int64_t p : probes )
            CHECK( m.warpedToSrc( m.srcToWarped( Fraction( p ) ) )
                       == Fraction( p ),
                   "no-anchor roundtrip exact" );
    }

    // --- Anchor map: continuity, exact inverse, slope extension ----------
    {
        std::vector<twWarpAnchor> a = {
            { 48000, 96000 },     // 2x up to here
            { 96000, 120000 },    // then 0.5x
            { 144000, 216000 },   // then 2x again
        };
        twWarpMap m( a, Fraction( 1 ) );
        CHECK( m.hasAnchors(), "anchors present" );

        // Passes exactly through the origin and every anchor.
        CHECK( m.srcToWarped( Fraction( 0 ) ) == Fraction( 0 ), "origin" );
        for( const twWarpAnchor &p : a ) {
            CHECK( m.srcToWarped( Fraction( p.src ) ) == Fraction( p.warped ),
                   "map passes through anchor (src->warped)" );
            CHECK( m.warpedToSrc( Fraction( p.warped ) ) == Fraction( p.src ),
                   "map passes through anchor (warped->src)" );
        }

        // Interior exactness: midpoint of segment 2 (src 72000 is halfway
        // 48000..96000 → warped halfway 96000..120000 = 108000).
        CHECK( m.srcToWarped( Fraction( 72000 ) ) == Fraction( 108000 ),
               "interior segment interpolates exactly" );

        // Beyond the last anchor the BASE stretch (1x here) resumes — NOT
        // the final segment's 2x. Editing localization depends on this:
        // material after the last marker rides rigidly with it.
        CHECK( m.srcToWarped( Fraction( 144000 + 1000 ) )
                   == Fraction( 216000 + 1000 ),
               "base-stretch extension beyond last anchor" );

        // Exact roundtrip everywhere, incl. non-integer rationals and the
        // extension regions.
        const Fraction probes[] = {
            Fraction( 1, 3 ), Fraction( 47999 ), Fraction( 48000 ),
            Fraction( 48001 ), Fraction( 95999, 2 ), Fraction( 100000 ),
            Fraction( 200000 ), Fraction( -100 ),
        };
        for( const Fraction &p : probes )
            CHECK( m.warpedToSrc( m.srcToWarped( p ) ) == p,
                   "anchor-map roundtrip exact (both directions)" );

        // Monotonicity spot check across every breakpoint.
        Fraction prev = m.srcToWarped( Fraction( -1 ) );
        for( int64_t s = 0; s <= 150000; s += 1500 ) {
            Fraction w = m.srcToWarped( Fraction( s ) );
            CHECK( w > prev, "strictly monotone src->warped" );
            prev = w;
        }
    }

    // --- Single anchor: base stretch resumes past it -----------------------
    {
        std::vector<twWarpAnchor> a = { { 100, 300 } };   // 3x up to here
        twWarpMap m( a, Fraction( 1 ) );
        CHECK( m.srcToWarped( Fraction( 200 ) ) == Fraction( 400 ),
               "single anchor: base stretch extends" );
        CHECK( m.warpedToSrc( Fraction( 750 ) ) == Fraction( 550 ),
               "single anchor: inverse in extension" );
    }

    // --- sanitize: deterministic offender dropping -------------------------
    {
        std::vector<twWarpAnchor> in = {
            { 300, 500 },
            { 100, 200 },
            { 300, 400 },     // duplicate src — dropped (after sort keeps first by warped)
            { 200, 150 },     // warped not increasing vs (100,200) — dropped
            { 0, 0 },         // not strictly positive vs origin — dropped
            { 400, 700 },
        };
        std::vector<twWarpAnchor> out = twWarpMap::sanitize( in );
        const std::vector<twWarpAnchor> expect = {
            { 100, 200 }, { 300, 400 }, { 400, 700 },
        };
        CHECK( out == expect, "sanitize keeps deterministic increasing chain" );
    }

    if( g_fails == 0 )
        std::cout << "\nAll twwarpmap tests passed.\n";
    else
        std::cout << "\n" << g_fails << " twwarpmap check(s) FAILED.\n";
    return g_fails;
}
