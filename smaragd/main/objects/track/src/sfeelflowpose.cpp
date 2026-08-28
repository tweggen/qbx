#include "app/objects/track/sfeelflowpose.h"

// For kSwayDeg / kNodDeg only -- see kNeckGain's note on why the pose has to
// know them. objects/track already declares `model` in check_layering's
// APP_DEPS, so this needs no new edge.
#include "app/model/sfeelflowskeleton.h"

#include <QString>

#include <algorithm>
#include <cmath>

namespace {

// The unit NAMES the parts are driven by, in SFeelFlowPose::Part order.
// By name, never by ensemble index -- see the header's point 3.
//
// PartReference IS DELIBERATELY EMPTY since proposal 44 C1. The head used to be
// driven by "reference" -- the tatum-rate gauge, the FASTEST unit in the
// ensemble -- which put it at 3.999 Hz against the trunk's 0.500 Hz, three
// octaves apart and statistically independent (corr +0.024). Its inertial
// torque was TEN TIMES its gravity term: driven 3.16x above its own
// inertia/gravity crossover of 1.264 Hz, i.e. flung rather than carried.
//
// It is NOT re-pointed at another unit, because every unit is already taken
// one-per-part: bounce->pelvis, sway->trunk, limbs->arms, twobar->hip. Any
// re-mapping would make two body parts draw the IDENTICAL series -- same
// power, same cosPhi, same displacement -- which is a loss of information, not
// a fix. The head is instead DERIVED from the trunk (deriveHeadNod below), and
// "reference" goes back to being the pure residual gauge section 3.2 designed
// it as. It is already surfaced as confidence/compliance in the panel, so
// nothing is lost from the readout -- only from the puppet.
const char *const kPartUnitName[SFeelFlowPose::PartCount] = {
    "bounce",      // PartBounce    -> pelvis vertical
    "sway",        // PartSway      -> torso lean
    "limbs",       // PartLimbs     -> arm swing (drawn in antiphase)
    "",            // PartReference -> DERIVED from the trunk, see above
    "twobar"       // PartTwobar    -> hip x-shift
};

// --- the neck, as a one-pole lag (proposal 44 C1, option D) ----------------
//
// The head's world angle is the trunk's plus a small LAGGING offset:
//
//     headWorld(t) = (1-g)*trunk(t) + g*lowpass(trunk, tau)(t)
//
// so what the pose carries -- headNod, which the skeleton applies RELATIVE to
// the trunk -- is g*(lowpass(trunk) - trunk), negative while the trunk is
// moving away and positive as it settles back. That is the head falling behind
// going in and catching up coming back, which is what "the head is subordinate
// to the torso" means kinematically.
//
// WHAT THIS IS NOT. A first-order lag is not a plant: there is no mass, no
// gravity, no overshoot and no arrest. C4 replaces it with the real
// second-order response, whose missing term is m*d*a_base. This is the
// smallest change that makes the head follow the trunk at all while costing no
// ensemble change and no aspect bump.
//
// tau is the head's own inertia/gravity CROSSOVER period, 1/(2*pi*1.264 Hz) --
// the frequency at which its inertial demand equals its weight moment. Below
// it a head is carried; above it, flung. Using it as the neck's time constant
// is the one principled number available before C2's measures exist.
constexpr double kNeckTauSec = 0.126;
constexpr double kNeckGain   = 0.5;    // the plan's bound; 0 would weld the
                                       // head to the trunk (C0's behaviour)

// The aspect hop is FIXED at rate/100 by twaspects.h ("groove.res" is a 10 ms
// hop whatever the sample rate), so dt is 0.01 s and does not need the rate --
// which SFeelFlowUiData does not carry.
constexpr double kHopSec = 0.01;

inline float clamp11( float v )
{
    if( !( v == v ) ) return 0.0f;    // NaN: a joint angle we cannot draw
    return std::max( -1.0f, std::min( 1.0f, v ) );
}

/** One unit's displacement at one hop -- `sqrt(power) * cosPhi`, the same
 * quantity the per-part loop computes, factored out so the neck's history walk
 * reads EXACTLY what the trunk part reads. Two spellings of this would be two
 * chances for the head to lag a trunk that is not the drawn one. */
inline double unitDispAt( const SFeelFlowUiData &ui, size_t unit, size_t hop )
{
    const size_t i = hop * ui.nUnits + unit;
    if( i >= ui.perUnitPower.size() || hop >= ui.dyn.size() ) return 0.0;
    if( unit >= ui.dyn[hop].units.size() ) return 0.0;
    const double power = ui.perUnitPower[i];
    const double e     = power > 0.0 ? std::sqrt( power ) : 0.0;
    return e * (double) ui.dyn[hop].units[unit].cosPhi;
}

/**
 * The head's excursion RELATIVE TO THE TRUNK at `hop`, from the trunk's own
 * history: `g * (lowpass(trunk, tau) - trunk)`, converted from the trunk's
 * display scale into the head's.
 *
 * O(hop) -- the one-pole is integrated from the start of the material on every
 * call, which is what keeps the pose a PURE function of the immutable snapshot
 * with no cached state anywhere. Measured cost is negligible: a 10-minute track
 * is 60000 hops of two array reads and three flops, well under 100 us, against
 * a repaint that happens at most ~30 times a second. Caching it would buy
 * microseconds and cost an invalidation protocol.
 *
 * THE DEGREE CONVERSION IS DELIBERATE AND IS THE ONE WART. `sway` is
 * normalized against kSwayDeg and `headNod` against kNodDeg, so expressing "the
 * head's world angle is the trunk's plus a lagging offset" -- an equation in
 * DEGREES -- requires their ratio. It couples the pose to two display
 * constants, which is why they are named here rather than duplicated: change
 * kNodDeg and the pinned pose numbers move, by design rather than by accident.
 */
inline double deriveHeadNod( const SFeelFlowUiData &ui, size_t swayUnit,
                             size_t hop, double frac )
{
    const double alpha = kHopSec / ( kNeckTauSec + kHopSec );
    double lag = unitDispAt( ui, swayUnit, 0 );
    for( size_t h = 1; h <= hop && h < ui.dyn.size(); h++ )
        lag += alpha * ( unitDispAt( ui, swayUnit, h ) - lag );

    // Interpolate the trunk the same way the per-part loop does, and advance
    // the lag by the same fraction of a hop, so the head does not step while
    // the trunk slides.
    const size_t hopNext = std::min( hop + 1, ui.dyn.size() - 1 );
    const double trunkA  = unitDispAt( ui, swayUnit, hop );
    const double trunkB  = unitDispAt( ui, swayUnit, hopNext );
    const double trunk   = trunkA + ( trunkB - trunkA ) * frac;
    const double lagI    = lag + alpha * ( trunkB - lag ) * frac;

    const double ratio = sfeelflowskel::kSwayDeg / sfeelflowskel::kNodDeg;
    return kNeckGain * ( lagI - trunk ) * ratio;
}

} // namespace

SFeelFlowPose sFeelFlowPoseAt( const SFeelFlowUiData &ui, offset_t frame )
{
    SFeelFlowPose pose;

    // --- the INVALID outcomes (header, first bullet) --------------------
    // Note the dyn.size() != compliance.size() check: a pre-M3e (v1) store
    // decodes to an EMPTY dyn -- the aspect version is part of the store key,
    // so a v1 entry misses rather than being decoded at the wrong stride --
    // and any other mismatch means the two payloads are not on one grid, in
    // which case indexing them with one hop number would silently pair a
    // power with somebody else's phase.
    if( ui.hopFrames == 0 || ui.nUnits == 0 || frame < 0 ) return pose;
    if( ui.dyn.empty() || ui.dyn.size() != ui.compliance.size() ) return pose;

    const size_t nHops = ui.dyn.size();

    // From here on the answer is VALID -- including the past-the-material
    // case, which is a real, distinguishable outcome (an all-zero pose that
    // is not a "no analysis" pose): the puppet stands still.
    pose.valid = true;

    const double h       = (double) frame / (double) ui.hopFrames;
    const size_t hop     = (size_t) h;
    if( hop >= nHops ) return pose;        // past the last hop: stand still

    const size_t hopNext = std::min( hop + 1, nHops - 1 );
    const float  frac    = (float) ( h - (double) hop );

    // Resolve each part's unit ONCE, by name. An absent name leaves the part
    // at 0 -- never a fallback to a neighbouring unit.
    for( int p = 0; p < SFeelFlowPose::PartCount; p++ ) {
        size_t unit = ui.unitNames.size();     // == "not found"
        for( size_t u = 0; u < ui.unitNames.size(); u++ ) {
            if( ui.unitNames[u] == kPartUnitName[p] ) { unit = u; break; }
        }
        if( kPartUnitName[p][0] == '\0' ) continue;   // derived, not mapped
        if( unit >= ui.unitNames.size() || unit >= ui.nUnits ) continue;

        // Power comes from the "groove.res" columns (hop-major), phase from
        // the "groove.dyn" record's own per-unit slice -- the two are on one
        // grid in one ensemble order, which is what the size check above is
        // protecting.
        const size_t iA = (size_t) hop     * ui.nUnits + unit;
        const size_t iB = (size_t) hopNext * ui.nUnits + unit;
        if( iA >= ui.perUnitPower.size() || iB >= ui.perUnitPower.size() )
            continue;
        const twGrooveDynRecord &recA = ui.dyn[hop];
        const twGrooveDynRecord &recB = ui.dyn[hopNext];
        if( unit >= recA.units.size() || unit >= recB.units.size() ) continue;

        const float powerA = ui.perUnitPower[iA];
        const float powerB = ui.perUnitPower[iB];
        const float power  = powerA + ( powerB - powerA ) * frac;

        // The raw, UN-renormalized bin-averaged cosine -- header point 2.
        const float cosA   = recA.units[unit].cosPhi;
        const float cosB   = recB.units[unit].cosPhi;
        const float cosPhi = cosA + ( cosB - cosA ) * frac;

        const float energy = power > 0.0f ? std::sqrt( power ) : 0.0f;
        const float disp   = clamp11( energy * cosPhi );

        pose.energy[p] = std::max( 0.0f, std::min( 1.0f, energy ) );
        switch( p ) {
            case SFeelFlowPose::PartBounce:    pose.bounceY  = disp; break;
            case SFeelFlowPose::PartSway:      pose.sway     = disp; break;
            case SFeelFlowPose::PartLimbs:     pose.armSwing = disp; break;
            case SFeelFlowPose::PartReference: pose.headNod  = disp; break;
            case SFeelFlowPose::PartTwobar:    pose.hipShift = disp; break;
            default: break;
        }
    }

    // --- the head, DERIVED from the trunk (C1 option D) --------------------
    // After the loop, so the trunk's own unit is already resolved and the head
    // can never be computed from a stale or absent sway.
    {
        size_t swayUnit = ui.unitNames.size();
        for( size_t u = 0; u < ui.unitNames.size(); u++ )
            if( ui.unitNames[u] == kPartUnitName[SFeelFlowPose::PartSway] ) {
                swayUnit = u; break;
            }
        if( swayUnit < ui.unitNames.size() && swayUnit < ui.nUnits ) {
            pose.headNod = clamp11( (float) deriveHeadNod( ui, swayUnit, hop, frac ) );
            // The head is exactly as "participating" as the trunk it hangs on,
            // so it borrows the trunk's energy rather than inventing one or
            // keeping the retired reference unit's.
            pose.energy[SFeelFlowPose::PartReference] =
                pose.energy[SFeelFlowPose::PartSway];
        }
    }

    return pose;
}

std::string sFeelFlowPoseDescribe( const SFeelFlowPose &pose )
{
    float energySum = 0.0f;
    for( int p = 0; p < SFeelFlowPose::PartCount; p++ ) energySum += pose.energy[p];

    // QString::number, never snprintf("%.4f"): the C library's %f honours
    // LC_NUMERIC, and this box's is de_DE -- a decimal COMMA would silently
    // break every `contains=` in the gate (and every parse downstream of it).
    // QString::number formats in the C locale unconditionally.
    auto f4 = []( float v ) { return QString::number( (double) v, 'f', 4 ); };
    const QString out =
        QStringLiteral( "pose: valid=%1 bounceY=%2 sway=%3 armSwing=%4"
                        " headNod=%5 hipShift=%6 energySum=%7" )
            .arg( pose.valid ? 1 : 0 )
            .arg( f4( pose.bounceY ), f4( pose.sway ), f4( pose.armSwing ),
                  f4( pose.headNod ), f4( pose.hipShift ), f4( energySum ) );
    return out.toStdString();
}
