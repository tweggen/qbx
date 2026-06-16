# Smaragd Project File Format

## Overview

Smaragd project files use XML format with exact rational arithmetic (fractions) for all time-related measurements. This ensures:
- **Precision**: No floating-point rounding errors, even with deeply nested structures
- **Composability**: Time-stretched clips containing time-stretched clips maintain exact timing
- **Reproducibility**: Undo/redo operations restore exact state, not approximations
- **Flexibility**: Fractions can be specified at any hierarchy level as needed

## Design Philosophy

### The Problem with Floating-Point Time

When audio structures are nested with time-stretching applied at multiple levels:
```
Project @ 48kHz
  └─ Track (stretched 1.5x)
      └─ Cut (stretched 2.0x)
          └─ Nested asset (stretched 1.33x)
```

Each operation compounds floating-point errors. After multiple transformations and undo/redo cycles, clip positions can be off by samples—invisible but destructive.

### The Rational Solution

All time-related values are stored as **fractions** (numerator/denominator). Arithmetic on fractions is exact:
- `240000/48000 × 3/2 = 360000/48000` (exact, no rounding)
- `44100/44100 × 2/1 = 88200/44100` (exact, no rounding)

This approach scales to arbitrary nesting depth without error accumulation.

## Time-Related vs. Non-Time-Related Values

### Time-Related (Use Fractions)
- **Positions**: when clips start on timeline
- **Durations**: how long clips are
- **Offsets**: which part of a source to play
- **Stretch factors**: time-stretching ratios
- **Playback rate/tempo**: timing ratios
- **Grain size**: processing window duration
- **Automation timepoints**: when automation events occur
- **Fade timing**: duration of crossfades/automation
- **Loop points**: start/end of loop regions

### Non-Time-Related (Float is Fine)
- **Volume**: amplitude in dB or linear
- **Pan**: left-right position
- **Pitch cents**: frequency offset (±1200 cents)
- **Metadata**: tags, names, descriptions

## Fraction Format

### Syntax

**Inline Fraction:**
```xml
position="240000/48000"
duration="480000"
stretch="3/2"
grainSizeMs="2048/48000"
```

**Parsing Rules:**
- `"240000"` → `240000/1` (bare integer)
- `"240000/48000"` → `240000/48000` (explicit fraction)
- `"5.0"` → `5/1` (decimal converted to fraction)
- `"1.5"` → `3/2` (known decimal mapped to fraction)
- `"42.6667"` → `2048/48000` (lookup table if available, else continued fractions)

### Simplification

Fractions are stored **in simplest form** (numerator and denominator have no common factors):
- `240000/48000` is simplified to... actually, let's check: gcd(240000, 48000) = 48000
  - Simplified: `5/1`
- `2048/48000` simplifies to `128/3000` (gcd = 16)

Simpler fractions are more human-readable in XML.

## Scope and Inheritance

### Project-Level Scope

The project specifies a base sample rate and time conversion factor:

```xml
<SProject sampleRate="48000" posFactor="1/48000">
  <!-- sampleRate: the project's native sample rate (informational) -->
  <!-- posFactor: how to convert positions to seconds -->
  <!-- Implicit: all child positions are in "sample units" (1/48000 sec) -->
```

### Local Scope Override

Any element can override the time conversion context with its own `posFactor`:

```xml
<SProject sampleRate="48000" posFactor="1/48000">
  
  <STrack posFactor="3/2">
    <!-- This track is stretched 1.5x -->
    <!-- Effective factor: (3/2) × (1/48000) = 3/96000 -->
    <!-- Positions here use 3/96000 as their time unit -->
    
    <SCut position="120000">
      <!-- This cut starts at 120000 × (3/96000) = 3.75 seconds -->
      <!-- But the position value itself, 120000, stays unchanged -->
    </SCut>
  </STrack>
  
</SProject>
```

### Factor Aggregation

When traversing the tree, factors **multiply**:
```
Effective time = position × (parent factor) × (element factor) × (project factor)

For nested stretches:
  position = 80000
  SCut factor = 2/1 (2x stretch within cut)
  Track factor = 3/2 (1.5x track-level stretch)
  Project factor = 1/48000 (base time unit)
  
  Time = 80000 × (2/1) × (3/2) × (1/48000)
       = 80000 × 2 × 3 / 2 / 48000
       = 240000 / 48000
       = 5 seconds
```

This is exactly like a graphics transformation stack (translation, rotation, scale).

## File Structure Example

```xml
<?xml version="1.0" encoding="UTF-8"?>
<SProject 
    fileName="my_project.smaragd"
    rootId="mixer_001"
    bpmTempo="120"
    sampleRate="48000"
    posFactor="1/48000"
    properties="{}">
  
  <!-- Root mixer -->
  <SStdMixer id="mixer_001" 
             duration="960000"
             muted="false"
             volume="0"
             pan="0">
    
    <!-- Background track -->
    <SLink objectId="wave_001" 
           hasStartTime="true" 
           startTime="0">
    </SLink>
    
    <!-- Arrangements track with clip -->
    <SLink objectId="track_001" 
           hasStartTime="true" 
           startTime="240000">
      <STrack id="track_001" 
              duration="720000"
              posFactor="3/2">
        <!-- 1.5x stretch applied to whole track -->
        
        <SLink objectId="cut_001" 
               hasStartTime="true" 
               startTime="0">
          <SCut id="cut_001"
                startOffset="0"
                cutDuration="480000"
                stretch="1/1"
                grainSizeMs="2048/48000"
                crossfadeMs="512/48000">
            <!-- Grain parameters stored as exact time fractions -->
            
            <SLink objectId="wave_001" 
                   hasStartTime="true">
            </SLink>
          </SCut>
        </SLink>
      </STrack>
    </SLink>
  </SStdMixer>
  
  <!-- Audio source -->
  <SPlainWave id="wave_001" 
              duration="960000"
              filename="background.wav"/>

</SProject>
```

## Action History (Undo/Redo)

Action files also use fractions for time-related fields:

```xml
<!-- Action: Move clip from position A to position B -->
<UndoAction type="move-clip"
            trackIndex="0"
            clipIndex="1"
            originalPos="240000"
            newPos="360000"
            <!-- Both positions use project's posFactor context -->
            duration="480000">
</UndoAction>

<!-- Action: Split clip at precise sample boundary -->
<UndoAction type="split-clip"
            clipPath="0/0"
            splitTime="120000/48000"
            <!-- Explicit fraction if different context -->
            originalDuration="480000"
            part1Duration="120000"
            part2Duration="360000">
</UndoAction>

<!-- Action: Create automated fade -->
<UndoAction type="add-automation"
            targetClip="0/0"
            automationPoints="
              120000/48000:0.0
              240000/48000:0.5
              360000/48000:1.0
            "
            <!-- Each point: time × value -->
            curveType="linear">
</UndoAction>
```

## Backward Compatibility

### Reading Legacy Files

Old files may have floating-point values instead of fractions. The parser is lenient:

```
Legacy Input              Parsed As                 Fraction
────────────────────────────────────────────────────────────
position="240000"        integer, no fraction      240000/1
position="5.0"           decimal float             5/1
position="42.6667"       known decimal table       2048/48000 (if known)
                         or continued fractions    else approx
duration="480000.5"      can't be exact sample     480000/1 (truncate)
                         warn user about precision loss
```

### Migration Strategy

1. **Read**: Parse legacy floats using continued fractions algorithm
2. **Approximate**: Convert to best rational approximation
3. **Store**: Save new file with exact fractions
4. **Notify**: Warn user if precision was lost in conversion

## Best Practices

### When to Use Which Format

**Bare integers** (use when position is in project sample units):
```xml
position="240000"          <!-- At 48kHz: 5 seconds -->
duration="480000"          <!-- 10 seconds -->
```

**Explicit fractions** (use for non-standard scales):
```xml
position="240000/44100"    <!-- 44.1kHz context -->
stretch="3/2"              <!-- Clear 1.5x factor -->
grainSizeMs="2048/48000"   <!-- Duration-based units -->
```

**Decimal format** (avoid in saved files, OK for manual editing):
```xml
<!-- Don't save as: -->
position="5.0"             <!-- Ambiguous: is it seconds? samples? -->

<!-- Better to save as: -->
position="240000"          <!-- Clear: project sample units -->
```

### Readability Tips

- Simplify fractions: `480000/48000` → `10/1`
- Use meaningful denominators: `115/100` (clearly 1.15x)
- Comment complex transformations:
  ```xml
  <!-- Track is 44.1kHz source, stretched 1.5x -->
  <STrack posFactor="66150/44100">
    <!-- (1.5 × 44100) / 44100 = 66150/44100 = 1.5x -->
  </STrack>
  ```

## Examples

### Example 1: Simple Project, No Stretching

```xml
<SProject sampleRate="48000" posFactor="1/48000">
  <STrack>
    <SCut position="240000" duration="480000">
      <!-- Position: 240000 × (1/48000) = 5 seconds -->
      <!-- Duration: 480000 × (1/48000) = 10 seconds -->
    </SCut>
  </STrack>
</SProject>
```

### Example 2: Nested Time-Stretching

```xml
<SProject sampleRate="48000" posFactor="1/48000">
  <STrack posFactor="3/2">
    <!-- 1.5x stretch -->
    <SCut position="160000" posFactor="4/3">
      <!-- Additional 1.33x stretch within cut -->
      <!-- Total: 1.5 × 1.33 = 2x -->
      <!-- Actual position: 160000 × (4/3) × (3/2) × (1/48000) -->
      <!--              = 160000 × 2 / 48000 = 6.67 seconds -->
    </SCut>
  </STrack>
</SProject>
```

### Example 3: Multi-Rate Content

```xml
<SProject sampleRate="48000" posFactor="1/48000">
  <STrack>
    <!-- 48kHz content, uses project factor -->
    <SCut position="240000" duration="480000"/>
  </STrack>
  
  <STrack sampleRate="44100" posFactor="1/44100">
    <!-- 44.1kHz content, different factor -->
    <SCut position="220500" duration="441000"/>
    <!-- Position: 220500 × (1/44100) = 5 seconds (same as above) -->
  </STrack>
</SProject>
```

### Example 4: Automation with Fractions

```xml
<SCut position="240000" duration="480000">
  <Automation parameter="volume">
    <Point time="0/1" value="0.0"/>            <!-- Start: silent -->
    <Point time="240000/480000" value="0.5"/>  <!-- 50% into clip: half volume -->
    <Point time="480000/480000" value="1.0"/>  <!-- End: full volume -->
  </Automation>
</SCut>
```

## Parsing Implementation

See `docs/FRACTION_PARSER.md` for implementation details:
- Parsing `"numerator/denominator"` format
- Converting decimals to fractions using continued fractions algorithm
- Handling edge cases and error conditions
- Lookup table for known decimal conversions

## Open Questions / Future Considerations

1. **Denominator size limits**: Should we cap the denominator to prevent huge numbers?
   - Currently: use continued fractions to approximate with small denominators
   - Max denominator: 1,000,000 (adjustable)

2. **Simplification**: Always store simplified fractions, or allow any form?
   - Current plan: always simplify via gcd() before saving
   - Improves readability and comparison

3. **Arithmetic precision**: When multiplying many fractions, denominator grows
   - Example: `(1/2) × (2/3) × (3/4) × (4/5) = 24/120 = 1/5`
   - Should we simplify after each operation?

## References

- **Continued Fractions**: Algorithm for converting decimals to rational approximations
- **Fraction Arithmetic**: Exact rational number operations without rounding
- **Transform Stacks**: Hierarchical factor composition (similar to graphics matrices)
