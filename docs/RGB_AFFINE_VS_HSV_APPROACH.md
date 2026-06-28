# RGB Affine vs. HSV Approach: Design Comparison

## Quick Comparison

| Aspect | **RGB Affine (Chosen)** | HSV Rotation |
|--------|------------------------|--------------|
| **Math** | `R' = f_r × R + o_r` per channel | Hue shift in HSV space |
| **Color Space Conversions** | None | RGB ↔ HSV on every paint |
| **Designer Experience** | "Add 35 to red channel" | "Rotate hue +60°" |
| **Wrapping Edge Cases** | None (simple clamping) | Hue wraps at 0°/360° |
| **Composability** | Factors multiply, offsets add | Hue addition modulo 360° |
| **Implementation Lines** | ~15 (simple linear math) | ~30 (HSV conversions) |
| **Tuning Process** | Use color picker → RGB values | RGB picker → HSV conversion → experiment |
| **Predictability** | Direct: factor/offset → output | Indirect: HSV rotation → RGB result |

## Why RGB Affine Wins

### 1. **Direct Control**
RGB affine lets you specify *exactly* what you want:
```cpp
// "I want to add warmth by increasing red and green"
offset_r = 35, offset_g = 35, offset_b = 0;  // ← Use color picker directly
```

HSV requires translation:
```cpp
// "I want a yellow tint"
// Find: which hue degree looks right? Test, iterate, convert to HSV
hue_offset_deg = 60.0f;
```

### 2. **No Conversions in Hot Path**
Painting happens ~60 FPS. Every track paints its background. RGB affine does:
```cpp
r = (int)(factor_r * r + offset_r);  // 1 multiply, 1 add
```

HSV approach does:
```cpp
int h, s, v;
baseColor.getHsv(&h, &s, &v);  // RGB → HSV conversion
// ... modify h, s, v ...
return QColor::fromHsv(h, s, v);  // HSV → RGB conversion
```

The conversions aren't free, and they're unnecessary overhead.

### 3. **Composability Without Modulo Arithmetic**
When both solo and armed, we choose solo:
```cpp
// RGB: Just overwrite offset
if (both_solo_and_armed) {
    offset_r = 35, offset_g = 35, offset_b = 0;  // Solo's yellow
}
```

HSV would need:
```cpp
// Hue: Modulo arithmetic to handle wrapping
h = (int)(h + 60.0f);  // Add solo's hue
h = ((h % 360) + 360) % 360;  // Normalize — extra logic
```

### 4. **Design Workflow**
**RGB approach:**
1. Open color picker
2. Pick a color you like (e.g., yellowish)
3. Copy RGB values into code
4. Done

**HSV approach:**
1. Open color picker, pick a color
2. Note the RGB values
3. Convert to HSV manually or with script
4. Extract hue offset
5. Put into code
6. Paint, see if it matches what you picked
7. Iterate

### 5. **Easier to Debug**
Print a modifier and see what it does:
```cpp
qWarning("Solo: offset_r=%d, offset_g=%d, offset_b=%d", 35, 35, 0);
// ← Immediately understand: "adding red+green = yellow"

// vs.

qWarning("Solo: hue_offset=%.1f°", 60.0f);
// ← Have to visualize what 60° in HSV looks like
```

## When Would HSV Be Better?

HSV makes sense if you're modeling colors *semantically* — e.g., "all blues, all reds, all yellows." But for affine transformations on an existing color, RGB is simpler.

Use HSV if:
- You want users to pick "solo color" from a palette (semantic choice)
- You're rotating through a hue wheel (e.g., assigning colors to categories)

Use RGB affine if:
- You're tinting/desaturating an existing color (our case)
- You want direct designer control
- You want minimal conversion overhead
