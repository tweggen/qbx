#ifndef _STRACK_COLOR_MODIFIER_H
#define _STRACK_COLOR_MODIFIER_H

#include <QColor>

class STrack;

/**
 * Track color modifier concept:
 *
 * A track's timeline background color is determined by:
 * 1. Base color (depends on selected state)
 * 2. State modifiers (muted, solo, armed/recording)
 *
 * This allows composable, predictable color transformations via affine
 * RGB transformations: C' = factor * C + offset, applied per-channel.
 *
 * The approach is functional: given a track state, generate the visual modifier
 * that should be applied to the base color.
 */

struct STrackColorModifier {
    // Affine RGB transformation: C' = factor * C + offset (per channel)
    // Enables desaturation (muted), color tinting (solo yellow, recording red)
    float factor_r = 1.0f, factor_g = 1.0f, factor_b = 1.0f;
    int offset_r = 0, offset_g = 0, offset_b = 0;

    /**
     * Derive a color modifier from the track's current state.
     * Combines modifiers from independent states (muted, solo, armed).
     *
     * Examples:
     * - Muted: factor=(0.7, 0.7, 0.7), offset=(30, 30, 30)
     *          Reduce saturation by scaling down and shifting to gray
     * - Solo: offset=(30, 30, 0) — add yellow tint
     * - Armed: offset=(30, 0, 0) — add red tint
     *
     * If multiple states are true, modifiers compose (factors multiply,
     * offsets add). Solo takes hue precedence over armed if both apply.
     */
    static STrackColorModifier fromTrackState(const STrack &track);

    /**
     * Apply this modifier to a base color, returning the transformed result.
     * Per-channel affine: C' = factor * C + offset, clamped to [0, 255].
     */
    QColor apply(const QColor &baseColor) const;
};

#endif
