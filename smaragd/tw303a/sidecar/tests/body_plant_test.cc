// Proposal 44 C5 -- THE PLANT PASS, over a REAL analysis.
//
// Everything proposal 44 built separately meets here, so this is the first
// gate that can fail for a reason none of the others can see. It runs the
// production front end and ensemble over a committed fixture, then the plant,
// and asserts what the CHAIN produces -- not what any one link does.
//
// THE CENTRAL ASSERTION IS SECTION 3 AND IT IS THE REQUESTER'S OWN. The head is
// driven by NO ensemble unit at all; it moves because the trunk moves, and it
// keeps going when the trunk stops. That is the defect this whole proposal
// started from (2026-08-27: "the head is subordinate in terms of movement to
// the bending of the torso" -- i.e. it was not), and it is the only assertion
// here that no amount of tuning could satisfy by accident.

#include "tw/sidecar/twbodyplant.h"
#include "tw/sidecar/twgrooveaspect.h"
#include "tw/sidecar/twgroovependulum.h"
#include "tw/body/twbodymeasures.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0;
static void check( bool ok, const char *what )
{ if( !ok ) { std::printf( "FAIL: %s\n", what ); g_fail++; } }   // check_logging: allow

/** A metrical click train: the same shape sidecar_test's groove section uses,
 * so the ensemble locks and every unit gets a real drive. */
static std::vector<float> clickTrain( uint32_t rate, double seconds,
                                      double period )
{
    std::vector<float> v( (size_t) ( seconds * rate ), 0.0f );
    for( double t = 0.0; t < seconds; t += period ) {
        const size_t at = (size_t) ( t * rate );
        for( size_t i = 0; i < rate / 100 && at + i < v.size(); i++ ) {
            const double env = std::exp( -(double) i / ( 0.004 * rate ) );
            // Broadband, so every spectral region sees it.
            v[at + i] += (float) ( 0.9 * env
                * std::sin( 2.0 * 3.14159265 * 220.0 * (double) i / rate )
                * ( 1.0 + 0.5 * std::sin( 2.0 * 3.14159265 * 3100.0
                                          * (double) i / rate ) ) );
        }
    }
    return v;
}

int main()
{
    const uint32_t rate = 48000;
    std::vector<float> mono = clickTrain( rate, 12.0, 0.25 );
    const float *chans[1] = { mono.data() };

    twGrooveAnalysisParams ap;
    twGrooveAspectPayloads pay =
        twGrooveBuildAspectPayloads( chans, 1, (uint64_t) mono.size(), rate, ap );
    check( pay.nUnits > 0, "the fixture locks -- there is an ensemble to drive with" );
    if( pay.nUnits == 0 ) { std::printf( "body_plant_test: 1 FAILURES\n" ); return 1; }   // check_logging: allow

    twBodyPlantInput in;
    in.nUnits = pay.nUnits;
    in.dyn    = twGrooveDecodeDynPayload( pay.dynPayload.data(),
                                          (uint64_t) pay.dynPayload.size(),
                                          pay.nUnits );
    for( const twGrooveCounterTension &ct : pay.counterTension ) {
        in.unitNames.push_back( ct.name );
        in.unitK.push_back( ct.k );
    }
    check( !in.dyn.empty(), "the dyn payload decodes" );
    check( in.unitK.size() == pay.nUnits && in.unitK[0] > 0.0,
           "and every unit's coupling reached the read side -- without it the"
           " plant cannot recover the music from the drive" );

    const twBodyMeasures body;                 // 75 kg, 1.75 m
    twBodyPlantReport rep;
    std::vector<twBodyPoseRecord> pose = twBodyPlantRun( in, body, &rep );

    std::printf( "  plant: %u hops, %u driven DOF, peak urge %.4f,"            // check_logging: allow
                 " peak sway %.4f rad, peak head nod %.4f rad\n",
                 rep.hops, rep.drivenDof, rep.peakUrge,
                 rep.peakTrunkSway, rep.peakHeadNod );

    // --- 1. it produced a pose at all, on the right grid -------------------
    check( pose.size() == in.dyn.size(),
           "the plant emits exactly one record per analysis hop" );
    check( rep.drivenDof >= 3,
           "and found a unit for at least three DOF -- the ensemble's own"
           " names, never an index" );

    // --- 2. THE BODY MOVES, and it moves in a bounded way ------------------
    {
        double peak[(int) twBodyPoseDof::Count] = {};
        bool finite = true;
        for( const twBodyPoseRecord &r : pose )
            for( int d = 0; d < (int) twBodyPoseDof::Count; d++ ) {
                const double a = (double) r.dof[d].angle;
                finite = finite && a == a && std::fabs( a ) < 1e3;
                peak[d] = std::max( peak[d], std::fabs( a ) );
            }
        check( finite, "every angle is finite -- no joint ran away" );
        for( int d = 0; d < (int) twBodyPoseDof::Count; d++ ) {
            const twBodyPoseDof dof = (twBodyPoseDof) d;
            std::printf( "    dof %d peak %.4f rad against a %.2f rad range\n",   // check_logging: allow
                         d, peak[d], twBodyPoseRangeRad( dof ) );
        }
        check( peak[(int) twBodyPoseDof::Sway] > 1e-4,
               "the TRUNK is moving -- the music reached a joint" );
        check( peak[(int) twBodyPoseDof::BounceY] > 1e-4, "and so is the leg" );
        for( int d = 0; d < (int) twBodyPoseDof::Count; d++ )
            check( peak[d] <= twBodyPoseRangeRad( (twBodyPoseDof) d ) * 3.0,
                   "and no DOF is wildly outside its own range" );
    }

    // --- 2a. THE TORQUE SCALE IS THE CLOSED FORM, asserted AS one ----------
    // **AN EXCURSION BOUND CANNOT CATCH A WRONG SCALE, and that was measured:**
    // replacing the derivation with a flat 60 N*m changed peak sway from 0.0542
    // to 0.0793 rad -- comfortably inside any plausible bound on a peak angle,
    // and the first version of this section passed under exactly that sabotage.
    // A closed form has to be asserted as a closed form.
    {
        const twBodyMeasures m;
        twBodyJoint trunk;
        trunk.segment          = m.segment( twBodySeg::Trunk );
        trunk.freeAxes         = twBodySpherical();
        trunk.invertedPendulum = true;
        trunk.parentLever      = m.compound( twBodySeg::Thigh, 3 ).length;
        trunk.stiffnessScale   = 2.0;
        trunk.dampingRatio     = 0.30;
        trunk.posturalGain     = 1.0;
        const twBodyParentMotion still;
        const double k    = twBodyJointStiffness( trunk, twBodyAxis::Lateral );
        const double zEff = twBodyJointDampingRatioAt( trunk, twBodyAxis::Lateral, still );
        const double want = twBodyPoseRangeRad( twBodyPoseDof::Sway ) * k
                            * ( 2.0 * zEff );          // rom * k / Q, Q = 1/(2z)
        const double got  = rep.torqueScale[(int) twBodyPoseDof::Sway];
        std::printf( "  torque scale for sway: %.6f N m (closed form %.6f)\n",   // check_logging: allow
                     got, want );
        check( std::fabs( got - want ) <= 1e-9,
               "THE TORQUE SCALE IS rom*k/Q EXACTLY -- full urge at resonance"
               " is full range of motion, derived and not chosen" );
        check( got > 0.0, "and it is positive" );
    }

    // --- 2b. THE PLANT'S URGE IS THE MUSIC, coupling divided out -----------
    // Also unreachable from an excursion: leaving the coupling in changes every
    // amplitude but breaks no bound. Recomputed here from the SAME dyn records
    // and compared exactly, which is the only way the divide is observable at
    // this level.
    {
        // Only the units the plant actually MAPS. "reference" is deliberately
        // driven to nothing (proposal 44 C1 retired it as a body driver and
        // gave it back to the residual gauge), and it has the highest urge on
        // this fixture -- 0.1705 against the mapped peak of 0.1621 -- so a
        // recompute over every unit reads high and this check failed on its
        // first run for that reason rather than for a real one.
        static const char *kMapped[] = { "bounce", "twobar", "sway", "limbs" };
        double want = 0.0;
        for( uint32_t u = 0; u < in.nUnits; u++ ) {
            bool mapped = false;
            for( const char *nm : kMapped )
                mapped = mapped || ( u < in.unitNames.size() && in.unitNames[u] == nm );
            if( !mapped ) continue;
            twBodyUrgeSmoother sm;
            for( const twGrooveDynRecord &r : in.dyn ) {
                if( u >= r.units.size() ) continue;
                want = std::max( want,
                    sm.step( twBodyDrive( r.units[u].support, r.units[u].tension,
                                          in.unitK[u] ), in.dtSec ) );
            }
        }
        std::printf( "  peak urge: plant %.6f, recomputed %.6f\n",               // check_logging: allow
                     rep.peakUrge, want );
        check( std::fabs( rep.peakUrge - want ) <= 1e-9,
               "THE PLANT'S URGE IS twBodyDrive WITH THE COUPLING DIVIDED OUT --"
               " leaving k in inflates it by the ratio of the couplings and"
               " breaks no excursion bound" );
    }

    // --- 2c. EACH DOF MOVES AT ITS OWN METRICAL RATE -----------------------
    // **C1a's finding, asserted at the top of the chain for the first time.**
    // `cosPhi` is arg(z), the DRIVE-LOCKED phase: for a unit driven far off its
    // resonance it is dominated by the forced response, so cos(arg z)
    // oscillates at the DRIVE rate however slow the unit is -- which is what
    // made every body part move at roughly the tatum, the defect the requester
    // reported. `cosMet` advances at the unit's OWN adaptive omega.
    //
    // **AND THIS SECTION DOES NOT GATE cosMet EITHER. MEASURED, AND THE REASON
    // IS INTERESTING.** Swapping cosMet for cosPhi moves the drawn rates from
    // 0.625 / 1.958 Hz to 0.542 / 2.583 Hz -- nowhere near the tatum, and
    // inside every band below. **The plant FILTERS THE DEFECT OUT**: a joint is
    // itself a resonator with a low natural frequency, so a 4 Hz forcing
    // produces a response dominated by the joint's own omega whatever phase
    // channel drove it. That is a real property of the plant and worth knowing
    // -- it is strictly more robust to C1a's defect than the direct mapping was,
    // which is exactly why the direct mapping needed C1a and this does not.
    //
    // So the phase-channel choice is gated where it is OBSERVABLE, at C1a's own
    // direct mapping, and NOT here. What this section does gate is worth having
    // on its own: that the metrical LADDER survives the plant -- the slow unit's
    // DOF stays slow, the quick one's stays quicker, and neither is dragged up
    // to the tatum by the machinery in between.
    {
        auto rateOf = [&]( twBodyPoseDof dof ) {
            int crossings = 0;
            for( size_t h = 1; h < pose.size(); h++ ) {
                const float a = pose[h-1].dof[(int) dof].angle;
                const float b = pose[h  ].dof[(int) dof].angle;
                if( ( a < 0.0f ) != ( b < 0.0f ) ) crossings++;
            }
            const double secs = (double) pose.size() * in.dtSec;
            return 0.5 * (double) crossings / secs;      // Hz
        };
        const double fSway   = rateOf( twBodyPoseDof::Sway );
        const double fBounce = rateOf( twBodyPoseDof::BounceY );
        // The fixture's own click period IS the tatum, by construction: it is
        // a 0.25 s train and the ensemble recovers exactly that.
        const double tatumHz = 1.0 / 0.25;
        std::printf( "  drawn rates: sway %.3f Hz, bounce %.3f Hz"               // check_logging: allow
                     " (the fixture's tatum is %.3f Hz)\n",
                     fSway, fBounce, tatumHz );
        // The sway unit is seeded EIGHT tatums, i.e. an eighth of the tatum
        // rate. Under the drive-locked phase it would run at roughly the tatum
        // instead -- an order of magnitude out, which is why a band this wide
        // is still a decisive test.
        check( fSway < tatumHz * 0.5,
               "THE TRUNK MOVES AT ITS OWN METRICAL RATE, far below the tatum --"
               " the drive-locked phase would put it near the tatum instead" );
        check( fBounce < tatumHz * 0.9,
               "and so does the leg" );
        check( fSway > 0.0, "and it does move" );
        check( fSway < fBounce * 1.5,
               "with the slower unit's DOF no faster than the quicker one's --"
               " the metrical ladder survives the plant" );
    }

    // --- 3. THE REQUESTER'S TEST, END TO END -------------------------------
    // THE HEAD IS MAPPED TO NO UNIT. Its motion can only have come from the
    // trunk, through the neck's own inertia. This is the defect proposal 44
    // started from, asserted at the top of the whole chain for the first time.
    {
        check( rep.peakHeadNod > 1e-5,
               "THE HEAD MOVES -- and it is driven by NO ensemble unit, so its"
               " motion came from the trunk through the neck" );

        // And it is not merely a copy of the trunk. A rigid pair would give a
        // relative angle of exactly zero at every hop; a first-order lag would
        // give one that never leads. Correlate the two and require that they
        // are related but NOT identical.
        double sxx = 0.0, syy = 0.0, sxy = 0.0;
        size_t n = 0;
        for( const twBodyPoseRecord &r : pose ) {
            const double x = r.dof[(int) twBodyPoseDof::Sway].angle;
            const double y = r.dof[(int) twBodyPoseDof::HeadNod].angle;
            sxx += x * x; syy += y * y; sxy += x * y; n++;
        }
        const double corr = ( sxx > 0.0 && syy > 0.0 )
                          ? sxy / std::sqrt( sxx * syy ) : 0.0;
        std::printf( "  head/trunk correlation %.4f  (rigid would be exactly"   // check_logging: allow
                     " 1.0000)\n", corr );
        check( n > 0 && std::fabs( corr ) < 0.999,
               "and the head is NOT rigidly welded to the trunk -- a relative"
               " angle exists, which is what the requester could not see" );
    }

    // --- 4. DETERMINISM: the payload is a function of its inputs -----------
    {
        twBodyPlantReport rep2;
        std::vector<twBodyPoseRecord> again = twBodyPlantRun( in, body, &rep2 );
        std::vector<uint8_t> a, b;
        twBodyPoseEncode( pose, a );
        twBodyPoseEncode( again, b );
        check( a == b, "the plant is byte-deterministic across two runs" );
        check( a.size() == pose.size() * (size_t) twBodyPoseDof::Count * 3 * 4,
               "and the encoded payload is at the declared stride" );
    }

    // --- 5. THE BODY IS A PARAMETER, and a different one moves differently --
    // If M and H did not reach the trajectory there would be no reason for
    // body.pose to carry them in its params blob at all, and AC5.1's whole
    // nesting argument would be decoration.
    {
        twBodyMeasures big; big.massKg = 110.0; big.statureM = 1.95;
        twBodyPlantReport repBig;
        std::vector<twBodyPoseRecord> poseBig = twBodyPlantRun( in, big, &repBig );
        std::vector<uint8_t> a, b;
        twBodyPoseEncode( pose, a );
        twBodyPoseEncode( poseBig, b );
        std::printf( "  a 110 kg / 1.95 m body: peak sway %.4f vs %.4f rad,"    // check_logging: allow
                     " head nod %.4f vs %.4f\n",
                     repBig.peakTrunkSway, rep.peakTrunkSway,
                     repBig.peakHeadNod, rep.peakHeadNod );
        check( a != b,
               "A DIFFERENT BODY MOVES DIFFERENTLY -- which is what makes M and"
               " H key material for body.pose, and only for body.pose" );
    }

    // --- 6. SILENCE MOVES NOTHING ------------------------------------------
    // A body with no drive at all must come to rest, not drift. It is also the
    // one case where the whole chain has a closed form: zero drive, zero urge,
    // zero active torque, and every joint at its own equilibrium.
    {
        twBodyPlantInput quiet = in;
        for( twGrooveDynRecord &r : quiet.dyn )
            for( twGrooveUnitDynSample &u : r.units )
                { u.support = 0.0f; u.tension = 0.0f; u.cosMet = 0.0f; }
        twBodyPlantReport repQ;
        std::vector<twBodyPoseRecord> poseQ = twBodyPlantRun( quiet, body, &repQ );
        double worst = 0.0;
        for( const twBodyPoseRecord &r : poseQ )
            for( int d = 0; d < (int) twBodyPoseDof::Count; d++ )
                worst = std::max( worst, std::fabs( (double) r.dof[d].angle ) );
        std::printf( "  silence: peak |angle| %.3e rad, peak urge %.3e\n",       // check_logging: allow
                     worst, repQ.peakUrge );
        check( worst < 1e-9, "SILENCE MOVES NOTHING -- every joint stays at rest" );
        check( repQ.peakUrge < 1e-12, "and the music asks for nothing" );
    }

    if( g_fail == 0 ) std::printf( "body_plant_test: PASS\n" );                  // check_logging: allow
    else              std::printf( "body_plant_test: %d FAILURES\n", g_fail );   // check_logging: allow
    return g_fail ? 1 : 0;
}
