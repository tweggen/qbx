# Audio Sync Bug: startOffset_ Lost for Non-Looping Reads

**Status:** Root Cause Identified  
**Location:** `SCut::rebuildReader()` lines 119-133  
**Impact:** Non-looping plainwave cuts play from wrong position (2 seconds early in playback, 1 second early in render)

---

## The Bug

### Symptom
```
Playback:  Audio 2 seconds early (1 bar) vs. visual cursor
Rendering: Audio 1 second early (2 beats) vs. visual cursor
Cut type:  SPlainWave (non-looping, non-stretched)
```

### Root Cause: startOffset_ Ignored in Non-Looping Path

**File:** `main/src/scut.cpp` lines 111-134

```cpp
// Line 119-123: Calculate adjusted start offset
offset_t adjustedStartOffset = snap.startOffset;  // ← Computed here
length_t adjustedLoopLength = snap.loopLength;
if( newGrain ) {
    adjustedStartOffset = (offset_t)llround( (double)snap.startOffset * snap.grainParams.stretch );
    adjustedLoopLength = (length_t)llround( (double)snap.loopLength * snap.grainParams.stretch );
}

// Line 125-133: Two paths
if( snap.loopLength > 0 && snap.loopLength < snap.cutDuration ) {
    // LOOPING PATH: Uses adjustedStartOffset ✓
    twLoopReader *lr = new twLoopReader( env, *view, adjustedStartOffset, adjustedLoopLength );
    lr->init();
    newReader = std::shared_ptr<twSampleReader>( lr );
    newLooping = true;
} else {
    // NON-LOOPING PATH: Ignores adjustedStartOffset ✗
    newReader = std::shared_ptr<twSampleReader>( view->acquireReader( env ) );
    newLooping = false;
    // adjustedStartOffset is calculated but NEVER USED!
}
```

### The Problem

**Looping cuts:** startOffset is PASSED to twLoopReader constructor
```cpp
twLoopReader *lr = new twLoopReader( env, *view, adjustedStartOffset, adjustedLoopLength );
                                                  ↑ startOffset passed
```

**Non-looping cuts:** startOffset is IGNORED
```cpp
newReader = std::shared_ptr<twSampleReader>( view->acquireReader( env ) );
            // No adjustedStartOffset parameter! Reader doesn't know about slip offset
```

**Result:**
- Reader created with position = 0
- Should be position = adjustedStartOffset
- Plays from wrong part of source
- Offset by exactly startOffset_ amount

---

## Why This Causes the Observed Symptom

### Example: Plainwave Cut with 2-Second Slip Offset

```
Source audio (wav file):
  [0-2s: intro]  [2-4s: correct audio]  [4-6s: more]

Cut window:
  startOffset_ = 2 seconds (slip)
  Should play: [2-4s] from source

Current behavior:
  Reader starts at 0, not 2
  Plays: [0-2s] from source
  Result: Plays intro (2 seconds) before correct audio

Visual cursor:
  Shows cut at timeline position 0
  Audio plays from source position 0 (instead of 2)
  Audio is 2 seconds EARLY
```

### Why Render Shows ~1 Second Early

Rendering might have a different buffer size or start logic, causing slightly different offset manifestation.

---

## The Fix

### Option 1: Pass startOffset to acquireReader()

Modify `twRandomSource::acquireReader()` to accept initial offset:

```cpp
// Before
newReader = std::shared_ptr<twSampleReader>( view->acquireReader( env ) );

// After
newReader = std::shared_ptr<twSampleReader>( view->acquireReader( env, adjustedStartOffset ) );
```

Then in `twRandomSource::acquireReader()`:
```cpp
twSampleReader *twRandomSource::acquireReader( tw303aEnvironment &env, offset_t initialOffset )
{
    twSampleReader *r = new twSampleReader( env, *this );
    r->init();
    r->seekTo( initialOffset );  // Start at correct position
    return r;
}
```

### Option 2: Seek After Creation

```cpp
// Non-looping path
newReader = std::shared_ptr<twSampleReader>( view->acquireReader( env ) );
newReader->seekTo( adjustedStartOffset );  // ← ADD THIS LINE
newLooping = false;
```

### Option 3: Unified Reader Creation

Create readers through a helper that always applies startOffset:

```cpp
std::shared_ptr<twSampleReader> createReader(
    tw303aEnvironment &env,
    twRandomSource *view,
    offset_t startOffset,
    offset_t loopStart,
    length_t loopLength
) {
    if( loopLength > 0 && loopLength < cutDuration_ ) {
        twLoopReader *lr = new twLoopReader( env, *view, startOffset, loopLength );
        lr->init();
        return std::shared_ptr<twSampleReader>( lr );
    } else {
        auto reader = std::shared_ptr<twSampleReader>( view->acquireReader( env ) );
        reader->seekTo( startOffset );  // Always apply offset
        return reader;
    }
}
```

---

## Why This Bug Exists

The code was written with the assumption that startOffset would be handled uniformly, but:

1. **twLoopReader constructor** explicitly takes startOffset (it's built in)
2. **twSampleReader::acquireReader()** returns a reader starting at position 0
3. The non-looping path forgot to seekTo() the initial offset

This is a **classic initialization omission**: the variable is computed but one code path doesn't use it.

---

## Verification

**To confirm this is the issue:**

1. Load project with plainwave cut with non-zero startOffset (slip)
2. Play it: observe audio starts 2 seconds early
3. Check startOffset_ in debugger: should be ~2 seconds
4. Check reader initial position: should be 0 (not startOffset_)

**Fix verification:**

1. Apply fix (seekTo after acquireReader)
2. Play same cut: audio should now align with cursor
3. Looping cuts should also still work (they already have the offset)

---

## Related Code Locations

- **SCut::rebuildReader():** `main/src/scut.cpp:111-134` (where bug is)
- **twLoopReader constructor:** Takes initial offset (correct)
- **twSampleReader::acquireReader():** Returns reader at position 0 (needs seekTo after)
- **SCut::seekTo():** Lines 515-532 (how offset is applied during playback, but too late)

The seekTo() at line 530 is correct for user seek operations, but the reader's **initial** position is never set to startOffset.

