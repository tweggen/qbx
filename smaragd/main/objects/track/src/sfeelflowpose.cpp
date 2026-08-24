#include "app/objects/track/sfeelflowpose.h"

#include <QString>

#include <algorithm>
#include <cmath>

namespace {

// The unit NAMES the five parts are driven by, in SFeelFlowPose::Part order.
// By name, never by ensemble index -- see the header's point 3.
const char *const kPartUnitName[SFeelFlowPose::PartCount] = {
    "bounce",      // PartBounce    -> pelvis vertical
    "sway",        // PartSway      -> torso lean
    "limbs",       // PartLimbs     -> arm swing (drawn in antiphase)
    "reference",   // PartReference -> head nod
    "twobar"       // PartTwobar    -> hip x-shift
};

inline float clamp11( float v )
{
    if( !( v == v ) ) return 0.0f;    // NaN: a joint angle we cannot draw
    return std::max( -1.0f, std::min( 1.0f, v ) );
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
