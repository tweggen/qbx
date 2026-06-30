# Grained Cut Offset Flow Trace - Root Cause Analysis

## Executive Summary

**ROOT CAUSE FOUND**: In `SCut::rebuildReader()`, when grain is active, `adjustedStartOffset` is NOT scaled by `snap.grainParams.stretch`, causing playback to start at the wrong position within the materialized stretched buffer.

**Evidence**: 
- Line 143 CORRECTLY scales `adjustedLoopLength` by stretch
- Lines 580-583 (seekTo) CORRECTLY scale offset by stretch
- But line 128-129 NEVER scales `adjustedStartOffset` for grain

---

## Offset Domain Mismatch

### The Two Domains

From comments in `scut.cpp:577-579`:
```
startOffset_ is in plainwave domain but the reader's underlying source
(twGrainSource) is in stretched domain.
```

**Plainwave domain**: Material position in the original, un-stretched source
- Example: sample 24000 of 48000 = "halfway through the material"

**Stretched domain**: Material position in the time-stretched output
- Example with stretch=2.0: halfway through = sample 48000 of 96000

**Conversion rule**: `offset_stretched = offset_plainwave * stretch`

---

## How Offsets Flow Through the Chain

### Part 1: Reader Construction (rebuildReader, lines 116-159)

```cpp
// Line 98: Get the source (SPlainWave returns twResampledSource @ project rate)
twRandomSource *rs = content_->getSObject().getRandomSource();

// Line 119: Create grain source with this source
if( !snap.grainParams.isIdentity() ) {
    newGrain = std::make_shared<twGrainSource>( *rs, snap.grainParams );
    view = newGrain.get();  // ← now reading through grain source
}

// Line 128: Initialize adjusted offset
offset_t adjustedStartOffset = snap.startOffset;  // ← PLAINWAVE DOMAIN (NO CONVERSION!)

// Line 141-145: Adjust loop length for stretch
if( newGrain ) {
    adjustedLoopLength = (length_t)llround( (double)snap.loopLength * snap.grainParams.stretch );
    // ✓ CORRECT: loop length scaled
    // ✗ BUG: startOffset NOT scaled here!
}

// Line 156: Pass offset to reader
newReader = std::shared_ptr<twSampleReader>( view->acquireReader( env, adjustedStartOffset ) );
//                                                                    ↑ STILL PLAINWAVE DOMAIN!
```

### Part 2: Seeking During Playback (seekTo, lines 577-583)

```cpp
offset_t startOff = snap.startOffset;
if (!snap.reader.looping && snap.reader.grain) {
    startOff = (offset_t)llround((double)startOff * snap.grainParams.stretch);
    // ✓ CORRECT: converts plainwave → stretched domain
}
offset_t seekPos = snap.reader.looping ? off : off + startOff;
if( snap.reader.reader ) return snap.reader.reader->seekTo( seekPos );
```

**KEY OBSERVATION**: The seek path correctly scales by stretch, but the rebuildReader path does NOT!

### Part 3: twGrainSource::read (twgrainsource.cc:84-113)

```cpp
length_t twGrainSource::read( offset_t srcOffset, sample_t *dest, length_t len, idx_t channel ) const
{
    // srcOffset is interpreted as position in STRETCHED domain (the materialized buffer)
    const sample_t *src = data_.data() + (size_t) ch * nFrames_ + srcOffset;
    memcpy( dest, src, sizeof( sample_t ) * n );
    return n;
}
```

The offset passed here MUST be in stretched domain.

---

## The Bug with Concrete Example

Scenario: 44.1 kHz WAV, 48 kHz project, stretch=2.0, startOffset=24000

```
1. Input source (twResampledSource @ 48 kHz):
   - length: 48000 samples = 1.0 second
   
2. twGrainSource after construction:
   - nFrames_: 48000 * 2.0 = 96000 samples
   - Materialized stretched buffer: 2.0 seconds of time-stretched audio
   
3. Current behavior (BROKEN):
   - adjustedStartOffset = 24000 (plainwave domain = halfway through)
   - twGrainSource::read(24000, ...) reads from sample 24000 of the 96000-sample buffer
   - That's position 0.25 seconds into the 2-second stretched buffer
   - ✗ WRONG: We wanted to start halfway through the material!
   
4. Expected behavior (CORRECT):
   - adjustedStartOffset should be: 24000 * 2.0 = 48000
   - twGrainSource::read(48000, ...) reads from sample 48000 of 96000
   - That's position 0.5 seconds (halfway) into the 2-second stretched buffer
   - ✓ CORRECT: Material position matches!
```

---

## Inconsistency Evidence

### Evidence 1: Loop length scaling (line 143)
```cpp
if( newGrain ) {
    adjustedLoopLength = (length_t)llround( (double)snap.loopLength * snap.grainParams.stretch );
```

Loop length IS scaled by stretch. Why would loop length need scaling but not start offset?

Because **both** must be in stretched domain when passed to twGrainSource!

### Evidence 2: seekTo scaling (lines 580-583)
```cpp
if (!snap.reader.looping && snap.reader.grain) {
    startOff = (offset_t)llround((double)startOff * snap.grainParams.stretch);
}
```

The seek path clearly states it needs to convert from plainwave to stretched domain.

Why wouldn't the initial reader construction need the same conversion?

### Evidence 3: Comment consistency
The comment at line 577 explains the domain mismatch exists. The fix at lines 580-583 handles it for seeking. The bug is that rebuildReader forgot to handle it for initial construction.

---

## Fix Location

**File**: `smaragd/main/src/scut.cpp`  
**Lines**: 141-149 (in the `if( newGrain )` block)  
**Change**: Add offset scaling to match loop length scaling

**Current (BROKEN)**:
```cpp
if( newGrain ) {
    adjustedLoopLength = (length_t)llround( (double)snap.loopLength * snap.grainParams.stretch );
    fprintf(stderr, "[SCut::rebuildReader] Grain: offset=%ld, adjustedLoop=%ld\n",
            (long)adjustedStartOffset, (long)adjustedLoopLength);
}
```

**Fixed**:
```cpp
if( newGrain ) {
    adjustedStartOffset = (offset_t)llround( (double)snap.startOffset * snap.grainParams.stretch );
    adjustedLoopLength = (length_t)llround( (double)snap.loopLength * snap.grainParams.stretch );
    fprintf(stderr, "[SCut::rebuildReader] Grain: offset=%ld, adjustedLoop=%ld\n",
            (long)adjustedStartOffset, (long)adjustedLoopLength);
}
```

This mirrors the conversion already done correctly in seekTo (line 582).
