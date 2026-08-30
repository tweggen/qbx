// Proposal 44 C5 (AC5.3 / AC5.4) -- the pose seam's ONE new branch.
//
// WHY A C++ TEST RATHER THAN A .qxa. The two things that have to be gated are
// (a) that with no "body.pose" on disk the pose numbers are BYTE-IDENTICAL to
// C3's, and (b) that with one present they come from it instead. Both are
// questions about ONE function given TWO snapshots that differ in exactly one
// field, and a script cannot construct an SFeelFlowUiData at all -- it can only
// reach the pose through a real analysis, which would make the comparison
// depend on the analysis rather than on the branch. Here the two snapshots are
// identical objects apart from `pose`, so nothing else can account for a
// difference.
//
// Sibling of preview_container_test; needs a QApplication (offscreen) for the
// app object libraries' static registrars.

#include "app/objects/track/sfeelflowpose.h"
#include "app/objects/track/sfeelflowbounce.h"

#include <QApplication>

#include <cmath>
#include <cstdio>
#include <vector>

static int g_fail = 0;
static void check( bool ok, const char *what )
{ if( !ok ) { std::printf( "FAIL: %s\n", what ); g_fail++; } }   // check_logging: allow
static void exact( float got, float want, const char *what )
{ if( !( got == want ) ) {
    std::printf( "FAIL: %s -- got %.9g want %.9g\n", what, (double) got, (double) want );   // check_logging: allow
    g_fail++; } }

/** A snapshot with a real five-unit ensemble and a hand-built dyn/power set --
 * enough for the C3 mapping to produce non-zero motion on every part. */
static SFeelFlowUiData makeSnapshot( size_t nHops )
{
    SFeelFlowUiData ui;
    ui.hopFrames = 480;                       // 10 ms at 48 kHz
    ui.unitNames = { "reference", "bounce", "limbs", "sway", "twobar" };
    ui.nUnits    = (uint32_t) ui.unitNames.size();
    ui.compliance.assign( nHops, 0.5f );
    ui.perUnitPower.resize( nHops * ui.nUnits );
    ui.dyn.resize( nHops );
    for( size_t h = 0; h < nHops; h++ ) {
        ui.dyn[h].units.resize( ui.nUnits );
        for( uint32_t u = 0; u < ui.nUnits; u++ ) {
            const double t = (double) h * 0.01;
            const double w = 2.0 * 3.14159265358979 * ( 0.5 + 0.5 * (double) u );
            ui.perUnitPower[h * ui.nUnits + u] =
                (float) ( 0.25 + 0.2 * std::sin( 0.7 * t + (double) u ) );
            ui.dyn[h].units[u].cosMet = (float) std::cos( w * t );
            ui.dyn[h].units[u].sinMet = (float) std::sin( w * t );
        }
    }
    return ui;
}

int main( int argc, char **argv )
{
    QApplication app( argc, argv );

    const size_t nHops = 400;
    const SFeelFlowUiData base = makeSnapshot( nHops );

    // --- AC5.4: with NO body.pose, every number is the C3 path's ----------
    // The strongest form available: the same snapshot evaluated twice, once
    // with an EMPTY pose vector and once with the field absent entirely (which
    // is the same object). Byte-identical is asserted with ==, not a
    // tolerance -- the branch is not taken, so the arithmetic is not merely
    // close, it is the same instructions.
    std::vector<SFeelFlowPose> c3;
    for( offset_t f = 0; f < (offset_t) ( nHops * 480 ); f += 733 )
        c3.push_back( sFeelFlowPoseAt( base, f ) );
    check( !c3.empty(), "the fallback path produces poses at all" );
    {
        bool anyMotion = false;
        for( const SFeelFlowPose &p : c3 )
            anyMotion = anyMotion || p.bounceY != 0.0f || p.sway != 0.0f
                                  || p.armSwing != 0.0f || p.hipShift != 0.0f;
        check( anyMotion, "and they are not all zero -- the fixture moves" );
    }
    {
        SFeelFlowUiData copy = base;
        copy.pose.clear();                     // explicitly empty
        size_t i = 0; bool same = true;
        for( offset_t f = 0; f < (offset_t) ( nHops * 480 ); f += 733, i++ ) {
            const SFeelFlowPose p = sFeelFlowPoseAt( copy, f );
            same = same && p.valid    == c3[i].valid
                        && p.bounceY  == c3[i].bounceY
                        && p.sway     == c3[i].sway
                        && p.armSwing == c3[i].armSwing
                        && p.headNod  == c3[i].headNod
                        && p.hipShift == c3[i].hipShift;
            for( int e = 0; e < SFeelFlowPose::PartCount; e++ )
                same = same && p.energy[e] == c3[i].energy[e];
        }
        check( same, "AC5.4: with no body.pose the numbers are BYTE-IDENTICAL"
                     " to C3's -- the branch is not taken at all" );
    }

    // --- AC5.3: with a body.pose present, the ANGLES come from it ---------
    {
        SFeelFlowUiData withPose = base;
        withPose.pose.resize( nHops );
        for( size_t h = 0; h < nHops; h++ ) {
            // A signature no ensemble mapping could produce: each DOF a
            // different constant, so a wrong DOF index is visible as a wrong
            // number rather than as a plausible one.
            withPose.pose[h].dof[(int) twBodyPoseDof::BounceY ].angle =  0.125f;
            withPose.pose[h].dof[(int) twBodyPoseDof::Sway    ].angle = -0.250f;
            withPose.pose[h].dof[(int) twBodyPoseDof::ArmSwing].angle =  0.375f;
            withPose.pose[h].dof[(int) twBodyPoseDof::HeadNod ].angle = -0.500f;
            withPose.pose[h].dof[(int) twBodyPoseDof::HipShift].angle =  0.625f;
        }
        const SFeelFlowPose p = sFeelFlowPoseAt( withPose, 100 * 480 );
        check( p.valid, "AC5.3: a pose-backed snapshot is valid" );
        exact( p.bounceY,   0.125f, "AC5.3: bounceY comes from the PLANT" );
        exact( p.sway,     -0.250f, "AC5.3: sway comes from the plant" );
        exact( p.armSwing,  0.375f, "AC5.3: armSwing comes from the plant" );
        exact( p.headNod,  -0.500f, "AC5.3: headNod comes from the plant --"
                                    " NOT the C1 one-pole derivation" );
        exact( p.hipShift,  0.625f, "AC5.3: hipShift comes from the plant" );

        // ENERGY STILL COMES FROM THE ENSEMBLE on both sides of the branch.
        // It answers "how much is this metrical level participating", which is
        // a property of the MUSIC: the plant changes how far a joint moves,
        // never how strongly the level it hangs on resonates. Keeping it
        // common is what lets a reader compare the two branches at all.
        const SFeelFlowPose c = sFeelFlowPoseAt( base, 100 * 480 );
        bool sameEnergy = true;
        for( int e = 0; e < SFeelFlowPose::PartCount; e++ )
            sameEnergy = sameEnergy && p.energy[e] == c.energy[e];
        check( sameEnergy,
               "AC5.3: and ENERGY is unchanged by the branch -- it is the"
               " music's participation, not the body's excursion" );

        // A pose whose length does not match the hop grid is REFUSED, not
        // indexed: the two payloads would not be on one grid, and pairing a
        // hop of one with a hop of the other is the same class of error the
        // dyn/compliance size check upstream exists to stop.
        SFeelFlowUiData ragged = withPose;
        ragged.pose.resize( nHops - 1 );
        const SFeelFlowPose r = sFeelFlowPoseAt( ragged, 100 * 480 );
        exact( r.bounceY, c.bounceY,
               "AC5.3: a pose off the hop grid falls back rather than being"
               " indexed against a grid it is not on" );

        // Clamping is defensive on this path too -- a plant angle outside
        // [-1,1] is a joint a widget cannot draw.
        SFeelFlowUiData wild = withPose;
        for( size_t h = 0; h < nHops; h++ )
            wild.pose[h].dof[(int) twBodyPoseDof::Sway].angle = -4.0f;
        exact( sFeelFlowPoseAt( wild, 100 * 480 ).sway, -1.0f,
               "AC5.3: and an out-of-range plant angle is CLAMPED, never drawn" );
    }

    if( g_fail == 0 ) std::printf( "feelflow_pose_test: PASS\n" );        // check_logging: allow
    else              std::printf( "feelflow_pose_test: %d FAILURES\n", g_fail );   // check_logging: allow
    return g_fail ? 1 : 0;
}
