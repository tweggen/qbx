# Track Color Modifier Concept

## Overview

The track timeline background color conveys multiple orthogonal pieces of state:
- **Selection** (darker blue when selected)
- **Mute** (grayscale/desaturated)
- **Solo** (yellow-ish tint)
- **Recording arm** (red-ish tint)

Rather than hardcoding conditional logic for each combination, we introduce a **color modifier** system using **affine RGB transformations** that compose these effects independently.

## Design Approach

### Affine RGB Transformation

We apply a per-channel linear transformation to the base color:

```
R' = factor_r × R + offset_r
G' = factor_g × G + offset_g
B' = factor_b × B + offset_b
```

This approach is:
- **Direct**: Designers specify exact RGB values, no color space conversions
- **Composable**: Factors multiply, offsets add naturally
- **Intuitive**: "Add red" = increase R offset, "desaturate" = reduce all factors

### Modifier Structure

```cpp
struct STrackColorModifier {
    float factor_r = 1.0f, factor_g = 1.0f, factor_b = 1.0f;
    int offset_r = 0, offset_g = 0, offset_b = 0;
};
```

Two static methods:
1. **`fromTrackState(const STrack&)`** — Query track state (muted, solo, armed), derive the modifier.
2. **`apply(const QColor&)`** — Transform a base color: C' = factor × C + offset, clamped to [0, 255].

### Visual Effects by State

| State | Effect | Parameters | Visual Result |
|-------|--------|------------|----------------|
| **Muted** | Desaturate + dim | `factor=(0.7, 0.7, 0.7), offset=(25, 25, 25)` | Gray, muted appearance |
| **Solo** | Add yellow warmth | `offset=(35, 35, 0)` | Warm yellow tint on all channels |
| **Armed** | Add red warning | `offset=(40, 0, 0)` | Warm red tint |
| **Muted + Solo** | Gray + yellow tint | Combine: factors reduce, yellow offset visible | Desaturated yellow-gray |
| **Muted + Armed** | Gray + red tint | Combine: factors reduce, red offset visible | Desaturated red-gray |
| **Solo + Armed** | Solo wins | Use solo's `offset=(35, 35, 0)` | Warm yellow (solo takes precedence) |
| **All three** | Muted + Solo | Combine mute (factor) with solo (offset) | Desaturated warm yellow |

**Rationale:** 
- Mute uses **factors** (multiplication) to reduce overall saturation/brightness
- Solo and armed use **offsets** (addition) to tint toward their respective colors
- When both armed and solo: solo's yellow offset takes precedence (solo is the louder signal)

## Integration Points

### 1. Track Timeline Renderer (`strackrndrinline.cpp`)

```cpp
// Determine base color (selection state)
bool isTrackSelected = /* ... */;
QColor baseColor = isTrackSelected 
    ? QColor(60, 90, 130)  // Selected blue
    : QColor(40, 70, 100); // Unselected blue

// Apply state modifier
STrackColorModifier mod = STrackColorModifier::fromTrackState(getTrack());
QColor finalColor = mod.apply(baseColor);

p.fillRect(visibRect, finalColor);
```

### 2. Track Header/Control Panel (`ssmvmixercontrol.cpp`)

The same modifier can be applied to the track header background for visual consistency:

```cpp
bgColor = selected_ 
    ? STrackColorModifier::fromTrackState(tk_).apply(QColor(64, 100, 140))
    : STrackColorModifier::fromTrackState(tk_).apply(QColor(48, 70, 100));
```

### 3. Future: Waveform Display

Waveform colors could also be tinted (e.g., solo = warmer colors), creating visual coherence across the entire track.

## Trade-Offs & Alternatives

### ✅ Chosen: Affine RGB Transformation (Factor + Offset)

**Pros:**
- **Direct control**: Designers specify exact RGB values they want (no color space conversions)
- **Composable**: Factors multiply (desaturation), offsets add (tinting)
- **Intuitive**: "Mute is gray" = reduce all factors and add gray offset
- **No wrapping issues**: Unlike hue rotation, no edge cases at 0°/360°
- **Lightweight**: Six floats (three factors, three offsets) per state
- **Easy to adjust**: Color picker → RGB values → directly into code

**Cons:**
- Slight cost from clamping after each transformation (negligible on modern CPUs)
- Designers must author RGB tuples rather than semantic concepts like "hue shift"

### ❌ Alternative 1: HSV-Based Hue Rotation

```cpp
float hue_offset_deg = 60.0f;  // Solo
float saturation_factor = 0.3f; // Mute
```

**Pros:**
- HSV is musician-friendly (hue = color tone, saturation = saturation)
- Reduces conceptual coupling between states

**Cons:**
- Requires HSV ↔ RGB conversion on every repaint (unnecessary overhead)
- Hue rotation can "wrap" unexpectedly near 0°/360°
- Harder to tune colors (design in RGB, convert to HSV, experiment)
- Per-channel affine is actually simpler once you see it

### ❌ Alternative 2: Semi-Transparent Overlay

Apply a semi-transparent colored overlay (e.g., 20% gray for mute, 30% yellow for solo).

**Pros:**
- Simple to understand (just an overlay)
- Predictable blending

**Cons:**
- Overlays wash out colors — solo track becomes pale, not vibrant
- Hard to combine overlays (overlay + overlay = muddy, unpredictable)
- Opacity management adds a parameter per state

### ❌ Alternative 3: Pre-Computed Color Palettes

Define a hard-coded palette for each state combination:

```cpp
struct TrackColors {
    QColor normal, selected, muted, solo, armed, muted_solo, ...
};
```

**Pros:**
- Designers have pixel-perfect control
- Lookup is O(1)

**Cons:**
- 2³ = 8 state combinations + selection state = 16 colors to maintain
- Adding new states (e.g., "archived") multiplies combinations further
- Impossible to scale coherently across the UI

## Implementation Plan

1. **Add `strackcolormodifier.h`** — Header with concept definition (factors + offsets)
2. **Add `strackcolormodifier.cpp`** — Implementation of affine RGB transforms
3. **Integrate into `strackrndrinline.cpp`** — Apply modifier to track background
4. **Integrate into `ssmvmixercontrol.cpp`** — Apply modifier to track header (optional, for consistency)
5. **Update CMakeLists.txt** — Add new source file
6. **Test** — Verify muted/solo/armed tracks display with correct colors

## Tuning Colors

RGB offsets and factors should be carefully chosen to:
- Be visually distinct (muted gray, solo yellow, recording red)
- Not clip into blacks or whites (clamp to [0, 255])
- Remain readable on both light and dark base colors

**Suggested tuning approach:**
1. Start with current values: `muted=(0.7f, 0.7f, 0.7f)+(25, 25, 25)`, `solo=+(35, 35, 0)`, `armed=+(40, 0, 0)`
2. Test on representative track colors (selected blue, unselected blue)
3. Adjust factors for saturation, offsets for hue/brightness
4. Consider storing these in `SProjectProps` for per-project theme support

## Future Extensions

- **Track volume indication**: Use `factor` to dim/brighten based on volume level
- **Per-project theme**: Store factor/offset pairs in project properties for branding
- **Accessibility mode**: Larger offsets for color-blind users (protanopia, deuteranopia)
- **Waveform tint**: Apply same modifiers to waveform colors in SCut renderer
- **Mute indicator**: Add subtle pattern/hatching overlay or icon for muted tracks
- **Record indicator**: Different offset while *actively recording* vs. just armed

## Code Review Checklist

- [ ] Affine math is correct (factor × C + offset)
- [ ] RGB values clamp correctly to [0, 255] (no overflow/underflow)
- [ ] Modifiers compose sensibly (muted + solo = desaturated yellow)
- [ ] Track header and timeline use consistent modifier logic
- [ ] No excessive allocations in hot path (painting on every frame)
