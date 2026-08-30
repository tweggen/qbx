#include "tw/body/twbodymeasures.h"

#include <cmath>

namespace {

constexpr double kPi = 3.14159265358979323846;

/**
 * The segment table, scaled from M and H.
 *
 * ================== EVERY ROW IS UNSOURCED. VERIFY. ==================
 * These are the values proposal 44 section 3 quotes "from memory of the
 * standard sources" (de Leva 1996's adjustment of Zatsiorsky-Seluyanov for the
 * mass/CoM/gyration columns; lengths after Drillis & Contini 1966). C2's AC2.1
 * exists to replace each `VERIFY` with an author/year/TABLE citation.
 *
 * SEARCHED AND REFUSED (2026-08-28), so the next person does not repeat it:
 * the de Leva 1996 PDF (ebm.ufabc.edu.br), Winter's anthropometry chapter
 * (courses.grainger.illinois.edu), the HAS-Motion Visual3D parameter wiki,
 * an NCBI/PMC article carrying an able-bodied reference table, ExRx's segment
 * table, arxiv, and Wikipedia -- ALL blocked by this environment's egress
 * proxy. Web SEARCH works but returns model-written summaries of those pages;
 * taking a number from a summary is laundering a memory rather than checking
 * it, so none was taken.
 *
 * The risk is not hypothetical. Section 4's arm range (0.9-1.1 Hz) is a
 * remembered number, and the closed form over THESE constants gives 0.7346 Hz
 * for the segment section 4 defines. A remembered number has already produced
 * one unresolvable gate in this plan.
 * =====================================================================
 */
struct Row {
    double massFrac;      // of M          VERIFY
    double lenFrac;       // of H          VERIFY
    double comFrac;       // of length, from proximal   VERIFY
    double gyrFrac;       // of length, TRANSVERSE about CoM   VERIFY
    /**
     * The segment's own RADIUS as a fraction of its length -- its slenderness.
     * This is NOT from the tables; it is the stated geometric model
     * twBodySegment::radiusGyrLong documents, so that a LONGITUDINAL inertia
     * exists at all rather than the transverse one being reused for twist.
     * A uniform solid cylinder of radius R has r_long = R/sqrt(2).
     *
     * ASSUMED, not sourced, and by a cruder route than the four columns to its
     * left -- those are remembered table values, this is a guess at a body's
     * proportions. Both are VERIFY; this one is the weaker of the two. It is
     * written down precisely so that it is visible and replaceable: the
     * standard sources carry a real longitudinal gyration column and AC2.1
     * substitutes it. Ordering sanity, which is the part that is NOT a guess:
     * the head is stubby (R/L large), the shank and upper arm are slender.
     */
    double girthFrac;     // R / length    VERIFY (assumption, see above)
};

// Order must match twBodySeg.
const Row kRows[(int) twBodySeg::Count] = {
    /* HeadNeck */ { 0.069, 0.130, 0.500, 0.300, 0.35 },   // VERIFY all five
    /* Trunk    */ { 0.435, 0.288, 0.450, 0.370, 0.34 },   // VERIFY all five
    /* UpperArm */ { 0.027, 0.186, 0.580, 0.280, 0.14 },   // VERIFY all five
    /* Forearm  */ { 0.016, 0.146, 0.460, 0.280, 0.15 },   // VERIFY all five
    /* Hand     */ { 0.006, 0.108, 0.790, 0.300, 0.21 },   // VERIFY all five
    /* Thigh    */ { 0.142, 0.245, 0.410, 0.330, 0.19 },   // VERIFY all five
    /* Shank    */ { 0.043, 0.246, 0.450, 0.260, 0.12 },   // VERIFY all five
    /* Foot     */ { 0.014, 0.152, 0.500, 0.300, 0.15 },   // VERIFY all five
};

} // namespace

twBodySegment twBodyMeasures::segment( twBodySeg s ) const
{
    twBodySegment out;
    const int i = (int) s;
    if( i < 0 || i >= (int) twBodySeg::Count ) return out;
    const Row &r = kRows[i];
    out.mass         = r.massFrac * massKg;
    out.length       = r.lenFrac  * statureM;
    out.comFromProx  = r.comFrac  * out.length;
    out.radiusGyrCom = r.gyrFrac  * out.length;
    out.inertiaProx  = out.mass * ( out.radiusGyrCom * out.radiusGyrCom
                                    + out.comFromProx * out.comFromProx );
    // About the segment's OWN LONG AXIS. No parallel-axis term: the CoM sits on
    // that axis, so the distance from it is zero.
    out.radiusGyrLong = r.girthFrac * out.length / std::sqrt( 2.0 );
    out.inertiaLong   = out.mass * out.radiusGyrLong * out.radiusGyrLong;
    return out;
}

twBodySegment twBodyMeasures::compound( twBodySeg first, int count ) const
{
    twBodySegment out;
    if( count <= 0 ) return out;
    const int i0 = (int) first;
    double offset = 0.0;         // distance of this segment's proximal joint
                                 // from the compound's own hinge
    double massSum = 0.0, momentSum = 0.0, inertiaSum = 0.0, lenSum = 0.0;
    double longSum = 0.0;
    for( int k = 0; k < count; k++ ) {
        const int i = i0 + k;
        if( i >= (int) twBodySeg::Count ) break;
        const twBodySegment seg = segment( (twBodySeg) i );
        const double d = offset + seg.comFromProx;   // CoM from the HINGE
        massSum    += seg.mass;
        momentSum  += seg.mass * d;
        // Parallel axis about the compound's hinge, per segment.
        inertiaSum += seg.mass * ( seg.radiusGyrCom * seg.radiusGyrCom + d * d );
        longSum    += seg.inertiaLong;
        lenSum     += seg.length;
        offset     += seg.length;
    }
    if( massSum <= 0.0 ) return out;
    out.mass        = massSum;
    out.length      = lenSum;
    out.comFromProx = momentSum / massSum;
    out.inertiaProx = inertiaSum;
    // The equivalent radius of gyration about the compound's own CoM, so the
    // struct stays self-consistent: I_prox = m*(r^2 + d^2).
    const double d2 = out.comFromProx * out.comFromProx;
    const double r2 = inertiaSum / massSum - d2;
    out.radiusGyrCom = r2 > 0.0 ? std::sqrt( r2 ) : 0.0;
    // Longitudinal inertia of a COLINEAR compound is a plain sum: every
    // segment shares the one long axis, so there is no parallel-axis term for
    // any of them. (A limb bent at the elbow does not, which is one more thing
    // the locked-straight-compound approximation gives up -- see the header.)
    out.inertiaLong  = longSum;
    out.radiusGyrLong = massSum > 0.0 ? std::sqrt( longSum / massSum ) : 0.0;
    return out;
}

double twBodyPendulumHz( const twBodySegment &seg )
{
    if( seg.inertiaProx <= 0.0 || seg.mass <= 0.0 ) return 0.0;
    const double num = seg.mass * twBodyGravity * seg.comFromProx;
    if( num <= 0.0 ) return 0.0;
    return std::sqrt( num / seg.inertiaProx ) / ( 2.0 * kPi );
}

double twBodyCrossoverHz( const twBodySegment &seg, double amplitudeRad )
{
    if( seg.inertiaProx <= 0.0 || amplitudeRad <= 0.0 ) return 0.0;
    const double num = seg.mass * twBodyGravity * seg.comFromProx
                       * std::sin( amplitudeRad );
    const double den = seg.inertiaProx * amplitudeRad;
    if( num <= 0.0 || den <= 0.0 ) return 0.0;
    return std::sqrt( num / den ) / ( 2.0 * kPi );
}
