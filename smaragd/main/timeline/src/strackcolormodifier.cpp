#include "app/timeline/strackcolormodifier.h"
#include "app/objects/track/strack.h"
#include <algorithm>

STrackColorModifier STrackColorModifier::fromTrackState(const STrack &track)
{
    STrackColorModifier mod;

    // Muted: desaturate by scaling RGB toward neutral gray and reducing intensity
    if (track.isMuted()) {
        mod.factor_r = 0.7f;
        mod.factor_g = 0.7f;
        mod.factor_b = 0.7f;
        mod.offset_r = 25;
        mod.offset_g = 25;
        mod.offset_b = 25;
    }

    // Solo: add yellow tint (increase R and G, keep B unchanged)
    if (track.isSolo()) {
        mod.factor_r = 1.0f;
        mod.factor_g = 1.0f;
        mod.factor_b = 1.0f;
        mod.offset_r = 35;
        mod.offset_g = 35;
        mod.offset_b = 0;
    }

    // Armed for recording: add red tint (increase R, reduce G and B slightly)
    if (track.isArmedForRecording()) {
        mod.factor_r = 1.0f;
        mod.factor_g = 1.0f;
        mod.factor_b = 1.0f;
        mod.offset_r = 40;
        mod.offset_g = 0;
        mod.offset_b = 0;
    }

    // If both solo and armed, solo takes hue precedence (warm yellow > red warning)
    if (track.isSolo() && track.isArmedForRecording()) {
        // Keep solo's yellow offset
        mod.offset_r = 35;
        mod.offset_g = 35;
        mod.offset_b = 0;
    }

    return mod;
}

QColor STrackColorModifier::apply(const QColor &baseColor) const
{
    int r = baseColor.red();
    int g = baseColor.green();
    int b = baseColor.blue();

    // Apply affine transformation per channel: C' = factor * C + offset
    r = (int)(factor_r * r + offset_r);
    g = (int)(factor_g * g + offset_g);
    b = (int)(factor_b * b + offset_b);

    // Clamp to [0, 255]
    r = std::max(0, std::min(255, r));
    g = std::max(0, std::min(255, g));
    b = std::max(0, std::min(255, b));

    return QColor(r, g, b);
}
