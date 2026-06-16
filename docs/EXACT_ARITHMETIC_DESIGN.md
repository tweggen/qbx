# Exact Arithmetic Design for Smaragd

## Problem Statement

Smaragd handles complex audio structures with multiple levels of nesting, time-stretching, and undo/redo. Using floating-point arithmetic for timing introduces cascading rounding errors:

- Stretch a clip by 1.5x (floating-point: 1.5000000000001)
- Nest it in another stretched group by 1.33x (floating-point: 1.3299999999999)
- Undo and redo the operation
- Result: Original position is off by samples—invisible but destructive

## Solution: Exact Rational Arithmetic

All time-related values are represented as **fractions** (exact rational numbers):
- **Positions**: Where clips start on timeline
- **Durations**: How long clips are
- **Stretch factors**: Time-stretching ratios
- **Processing timings**: Grain size, automation points, fades

This ensures **perfect precision** through arbitrary nesting, stretching, and undo/redo cycles.

## Key Design Decisions

### 1. Fractions for All Time-Related Values

✓ Use fractions: positions, durations, stretches, tempos, grain sizes, automation points  
✗ Don't use fractions: volume, pan, pitch cents (frequency offset, not time-based)

### 2. Hierarchical Factor Composition

Time factors aggregate as you descend the element tree:

```
Effective time = position × (posFactor at level 1) × (posFactor at level 2) × ...

Example:
  <Project posFactor="1/48000">
    <Track posFactor="3/2">          <!-- 1.5x stretch -->
      <Cut posFactor="2/1">          <!-- 2x within-cut stretch -->
        position="80000"
        
        Effective: 80000 × 2/1 × 3/2 × 1/48000 = 5 seconds
```

This is like a **graphics transformation stack** where each level multiplies the parent's factor.

### 3. Flexible Fraction Format

Users can specify fractions in various formats:
```xml
position="240000"           <!-- Bare integer (implicit /1) -->
position="240000/48000"     <!-- Explicit fraction -->
position="5.0"              <!-- Decimal, converted to 5/1 -->
stretch="3/2"               <!-- Stretch factor, exact 1.5x -->
grainSizeMs="2048/48000"    <!-- Grain size in time units -->
```

### 4. Flexible Parser

The parser is lenient—it handles legacy formats and best-effort approximations:
- Explicit fractions: parsed directly
- Known decimals: matched against lookup table for exact fractions
- Unknown decimals: approximated using continued fractions algorithm
- Bare integers: treated as `value/1`

## Documentation Structure

### [PROJECT_FILE_FORMAT.md](PROJECT_FILE_FORMAT.md)
**What:** The XML schema and format for project files

**Covers:**
- Design philosophy
- Time-related vs. non-time-related values
- Fraction syntax and parsing rules
- Scope and inheritance of posFactor
- Factor aggregation (multiplication)
- Example XML structures
- Action history format
- Backward compatibility strategy

**When to read:** Understanding how to save/load projects with exact timing

### [FRACTION_PARSER.md](FRACTION_PARSER.md)
**What:** Implementation details for parsing fractions from various formats

**Covers:**
- Parsing algorithm (explicit fractions, known decimals, continued fractions)
- Continued fractions algorithm for approximating decimals
- Complete C++ implementation with code examples
- Lookup table for known conversions
- Integration into XML reading
- Test cases
- Performance analysis
- Known limitations

**When to read:** Implementing the parser functions

## Implementation Roadmap

### Phase 1: Core Infrastructure
- [ ] Implement `Fraction` struct with arithmetic operators
- [ ] Implement `parseFractionOrDouble()` parser function
- [ ] Implement continued fractions algorithm
- [ ] Add GCD simplification
- [ ] Write unit tests for parser

### Phase 2: Project Serialization
- [ ] Update `SProject::serializeSelfAttributes()` to write `posFactor`
- [ ] Update `SProject::readPreChildrenAttributes()` to read `posFactor` and `sampleRate`
- [ ] Add posFactor aggregation in context stack

### Phase 3: Object Serialization
- [ ] Update `SCut::serializeSelfAttributes()` for grain parameters (already time-based)
- [ ] Update `SLink::serializeSelfAttributes()` for positions
- [ ] Update `SObject::serializeSelfAttributes()` for duration
- [ ] Update read methods to use new parser

### Phase 4: Action Files
- [ ] Update all action classes to serialize positions as fractions
- [ ] Update `writeXml()` methods
- [ ] Update `readXml()` methods
- [ ] Add support for posFactor context in action application

### Phase 5: Testing & Migration
- [ ] Write comprehensive tests for fraction arithmetic
- [ ] Test nested structures and factor aggregation
- [ ] Test backward compatibility with old files
- [ ] Create migration tool (optional)

## Example: Before and After

### Before (Floating-Point)

```xml
<SCut position="240000.0" 
      duration="480000.0"
      stretch="1.5"
      grainSizeMs="42.6667">
</SCut>

<!-- Problems:
  - 1.5 is approximate (binary representation)
  - grainSizeMs as float loses precision
  - Nested stretches compound errors
  - Undo/redo might not restore exactly
-->
```

### After (Exact Fractions)

```xml
<SCut position="240000"
      duration="480000"
      stretch="3/2"
      grainSizeMs="2048/48000">
</SCut>

<!-- Benefits:
  - All values exact (no rounding)
  - 3/2 is precisely 1.5x
  - Grain size preserved exactly
  - Nesting and undo/redo stay exact
-->
```

## Key Invariants

1. **Factor Multiplication Composability**
   ```
   (a/b) × (c/d) = (a×c)/(b×d)   -- Always exact
   ```

2. **Simplified Fractions**
   ```
   240000/48000 → simplified to 5/1
   2048/48000   → simplified to 128/3000 (gcd = 16)
   ```

3. **Position Independence from Factor**
   ```
   Position stays as integer "240000"
   Factor applies on interpretation: 240000 × posFactor
   Changing posFactor doesn't require position recalculation
   ```

4. **No Floating-Point in Temporal Domain**
   ```
   Time values: always fractions (exact)
   Stretch factors: always fractions (exact)
   Processing params: can be float (amplitude, pan, pitch cents)
   ```

## Migration Strategy

**For Existing Projects:**
1. Read old format (with floats)
2. Parse floats as approximate values using continued fractions
3. Save in new format (with exact fractions)
4. Verify precision: compare old and new playback

**For Undo/Redo History:**
1. Old actions with float positions are read and approximated
2. New actions always use exact fractions
3. Can mix old and new actions in same session

## Performance Impact

- **Parsing**: Continued fractions algorithm: O(log denominator), typically 5-10 iterations
- **Arithmetic**: Fraction operations are integer math (fast)
- **Storage**: Slightly larger XML with fractions, but compresses well
- **Overall**: Negligible, no measurable impact

## Why This Design

### Why Not Just Use Fractions Everywhere?

We *do* use fractions everywhere for time-related values. This is the simplest, safest approach.

### Why Not Use a Single Global Sample Rate?

Sample rate can vary per track (44.1kHz source in 48kHz project) and can change due to transformations. Using a single global rate limits flexibility and introduces complex rescaling logic.

### Why Factor-Based Composition?

Factor composition (multiplication down the tree) matches how audio transformations work:
- Stretch a track: multiply all child positions by stretch factor
- No need to recalculate absolute positions
- Just like graphics transformation matrices
- Compositional and scalable

### Why Exact Fractions Over Floating-Point?

Floating-point errors compound with:
- Nesting (clip in stretched clip in stretched clip)
- Time-stretching at multiple levels
- Undo/redo cycles
- Multiple transformations

Exact arithmetic eliminates all rounding—better than "good enough" approximations.

## References and Further Reading

- **PROJECT_FILE_FORMAT.md** — Detailed schema and XML structure
- **FRACTION_PARSER.md** — Parser implementation and algorithm details
- **Continued Fractions** — https://en.wikipedia.org/wiki/Continued_fraction
- **Rational Arithmetic** — https://en.wikipedia.org/wiki/Rational_number

## Questions and Open Items

1. **Should we provide a validation/migration tool** to convert old projects?
2. **Should the UI display fractions** to users, or convert to decimal/seconds?
3. **What's the maximum reasonable denominator** we should accept? (Current: 1,000,000)
4. **How should automation curves represent timing** if we're moving to fractions?

---

**Status**: Design complete, ready for implementation  
**Last Updated**: June 16, 2025  
**Related Commits**: c40ded1, fa233cb, fc57da0 (earlier sample-rate fixes)
