# Detailed Offset Scenario: startOffset k through twGrainSource to twWavInput

## Setup
- File: 44.1 kHz WAV, 88200 samples (2 seconds)
- Project: 48 kHz
- Cut: SPlainWave with grain
- User action: Drag cut to startOffset = 24000 on 48 kHz timeline
- Grain: stretch = 2.0, pitch = 0 (no pitch shift)

## Questions to Answer
1. What domain is startOffset k = 24000 in?
2. How does this flow through twGrainSource?
3. Where does the actual audio data come from?

---

## SCENARIO: Build Phase (rebuildReader)

### Chain of Sources

**Original File (44.1 kHz)**
```
twSampleSource (raw file)
  length: 88200 samples @ 44.1 kHz = 2.0 seconds
  sampleRate: 44100
```

**Resampled to Project Rate (48 kHz)**
```
twResampledSource (via viewAtRate(48000))
  constructor: reads ALL 88200 @ 44.1 kHz from twSampleSource
               resamples to 48 kHz
  length: 96000 samples @ 48 kHz = 2.0 seconds
  sampleRate: 48000
```

**twWavInput returns this resampled source**
```
twWavInput::getSource() returns twResampledSource @ 48 kHz
```

**Grained**
```
twGrainSource created with twResampledSource @ 48 kHz
  Constructor:
    - inLen = rs->length() = 96000
    - nFrames_ = (int)(96000 * 2.0) = 192000
    - Calls rs->read(0, ..., 96000, c) for each channel
      This reads the ENTIRE resampled 48 kHz audio into buffer
    - Applies grain processing to create stretched output
    - data_ = 192000 samples of stretched audio @ 48 kHz
  length: 192000 samples @ 48 kHz = 4.0 seconds
  sampleRate: 48000
```

### Offset Handling

**User Timeline Position**
```
User sees 48 kHz timeline
User drags cut to: 24000 samples = 0.5 seconds
startOffset = 24000 (this goes into snap.startOffset)
```

**Question: What domain is this 24000 in?**

The UI timeline is 48 kHz, so startOffset = 24000 is "0.5 seconds on the 48 kHz timeline"

But is this:
- A) 24000 samples @ 48 kHz (project domain)?
- B) Position within the original material (plainwave domain)?

Looking at line 684 in scut.cpp:
```cpp
if( oldStretch > 0.0 && p.stretch > 0.0 && p.stretch != oldStretch ) {
    startOffset_ = (offset_t) llround( (double) startOffset_ * k );  // ← SCALES WITH STRETCH!
}
```

This scaling suggests startOffset is in PLAINWAVE domain (position in material).

**In our scenario**:
```
startOffset = 24000 (plainwave domain = "halfway through the material")

With the 44.1 kHz file:
  - Original material: 88200 @ 44.1 kHz
  - Halfway = sample 44100
  - But we're using the resampled 48 kHz version: 96000 @ 48 kHz
  - Halfway in material = sample 48000 @ 48 kHz
  
But startOffset = 24000, not 48000!

So what does 24000 represent?
```

This is confusing. Let me check if startOffset is actually supposed to be in the RESAMPLED domain or the ORIGINAL domain.

---

## Alternative Interpretation

What if:
- startOffset is in "plainwave domain" where "plainwave" = the resampled @ project rate version
- So startOffset = 24000 means "position 24000 in the 48 kHz resampled material"

Then:
```
startOffset = 24000 (in 48 kHz resampled domain)
adjustedStartOffset = 24000 * 2.0 = 48000 (in stretched domain)

twGrainSource::read(48000, ...) reads from position 48000 of 192000-sample buffer
  = position 0.25 seconds into 4-second stretched clip
  ✓ CORRECT (we wanted to start 0.5 seconds into 2-second clip, 
              which becomes 1.0 seconds into 4-second stretched clip... wait, that's wrong!)
```

Actually, wait. If we're starting halfway through a 2-second clip (position 1 second), and we stretch by 2x, we should start 2 seconds into the 4-second stretched output.

But if startOffset = 24000 @ 48 kHz = 0.5 seconds, that's halfway (1 second out of 2).
After stretch = 2.0, we want to start at 2 seconds into 4 seconds.
In samples: 2 * 48000 = 96000

But we calculated: 24000 * 2.0 = 48000, which is only 1 second into 4 seconds. ✗ WRONG

---

## The Real Problem

The scaling by stretch assumes:
```
offset_stretched = offset_plainwave * stretch
```

But what if startOffset is NOT in plainwave domain? What if it's in TIMELINE domain (absolute position)?

Then:
- startOffset = 24000 = absolute timeline position (0.5 seconds @ 48 kHz)
- We DON'T scale it by stretch!
- We just use it directly: 24000
- twGrainSource::read(24000, ...) = 0.25 seconds into stretched clip ✗ WRONG

But then the seekTo code wouldn't scale it either. Yet it does!

---

## The Missing Piece: What is "plainwave domain"?

Need to verify: Is "plainwave domain" the same as "48 kHz project domain" or is it "44.1 kHz original file domain"?

The grain source is built from twResampledSource @ 48 kHz, so all its offsets are @ 48 kHz.

If startOffset = k @ 48 kHz and we scale by stretch to get k*s @ 48 kHz, then:
- We're treating startOffset as "position in the 48 kHz material"
- After stretch, we want position k*s in the stretched buffer
- This is correct IF startOffset is truly in plainwave (48 kHz) domain

But what if startOffset is actually in the ORIGINAL 44.1 kHz domain?

Then:
```
startOffset = 24000 @ 44.1 kHz
= 24000 * (48000/44100) ≈ 26122 @ 48 kHz

adjustedStartOffset = 26122 * 2.0 = 52244
```

Or do we scale the startOffset BEFORE passing to grain?

```
startOffset = 24000 @ 44.1 kHz
adjustedStartOffset = 24000 * (48000/44100) * 2.0 ≈ 52244
```

This would mean startOffset is in 44.1 kHz domain and needs to be:
1. Converted to 48 kHz domain
2. Scaled by stretch
3. Passed to grain

But the current code doesn't do step 1!
