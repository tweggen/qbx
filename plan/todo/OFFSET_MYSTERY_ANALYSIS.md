# Offset Mystery: Why Grained Cuts Still Don't Work

## The Paradox

We applied the fix: `adjustedStartOffset = k * stretch`

This should work because:
- The ungrained path uses offset k directly and works correctly
- The grained path reads from a pre-materialized stretched buffer
- Scaling by stretch is the standard domain conversion

Yet testing shows it's still wrong.

---

## What We Know

### Working (Ungrained)
```
File: 44.1 kHz, N samples
Project: 48 kHz
startOffset: k (unknown domain)

Flow:
  twResampledSource @ 48 kHz (N_resamp samples, resampled from 44.1 kHz)
  → twSampleReader.pos_ = k
  → twResampledSource.read(k, ...) 
  → Works correctly
```

### Broken (Grained with our fix)
```
File: 44.1 kHz, N samples
Project: 48 kHz
startOffset: k (same unknown domain)
Grain: stretch = 2.0

Flow:
  twResampledSource @ 48 kHz (N_resamp samples)
  → twGrainSource( *twResampledSource, stretch=2.0)
    - Materializes: 2*N_resamp samples (stretched @ 48 kHz)
  → twSampleReader.pos_ = k * 2.0
  → twGrainSource.read(k*2.0, ...)
  → Still wrong!
```

---

## The Critical Unknown: What Domain is k In?

### Hypothesis A: k is in 48 kHz Project Domain
**Evidence for:**
- UI timeline is 48 kHz
- Ungrained path uses k directly with twResampledSource @ 48 kHz
- Makes mathematical sense

**If true:**
- Ungrained: offset k @ 48 kHz into 96000-sample 48 kHz buffer → ✓ Works
- Grained: offset k*2 @ 48 kHz into 192000-sample 48 kHz buffer → ✓ Should work
- But it doesn't! ✗

### Hypothesis B: k is in Original 44.1 kHz Domain
**Evidence for:**
- Comment says "plainwave domain" which might mean "file domain"
- SCut XML serializes startOffset without rate conversion
- Sample rate is a property of the FILE, not the PROJECT

**If true:**
- Ungrained: offset k @ 44.1 kHz → twResampledSource converts internally ✓
- Grained: offset k @ 44.1 kHz → twGrainSource built from 48 kHz resampled source ✗
  - Mismatch! Need to convert k to 48 kHz first!

---

## The Test: How to Determine Domain

### Test Setup
- Create cut from 44.1 kHz file (88200 samples)
- Set startOffset = 44100 (halfway through)
- Play ungrained → where does audio start?
- Play grained with stretch=1.0 → where does audio start?

### Expected Results
**If k is in 44.1 kHz domain:**
- Both should start at 1.0 seconds (44100 samples @ 44.1 kHz = 1 second)
- In 48 kHz, that's ~47520 samples

**If k is in 48 kHz domain:**
- Both should start at 44100/48000 = 0.92 seconds
- That's 44100 samples (same value, but 0.92 seconds @ 48 kHz)

### Actual Behavior
Need to run this test and observe!

---

## Possible Root Cause #1: Missing Sample Rate Conversion

If k is in 44.1 kHz domain and we're applying grain (which is in 48 kHz domain):

```cpp
// Current (wrong if k @ 44.1 kHz):
adjustedStartOffset = k * stretch

// Should be:
int fileRate = 44100;  // Or dynamically query
int projectRate = 48000;
double rateRatio = (double)projectRate / fileRate;
adjustedStartOffset = (offset_t)llround((double)k * rateRatio * stretch);
```

This would explain why sample rate mismatch + grain = broken!

---

## Possible Root Cause #2: Capture Pages

The user mentioned "considering all of the capture pages inbetween".

There might be a capture page system that:
1. Caches audio at one rate
2. Supplies to grain source at different rate
3. Creates domain mismatch

We haven't analyzed the capture page flow yet!

---

## Next Steps for Diagnosis

### 1. Run the Domain Test
- Create 44.1 kHz file with distinctive content (tone sweep, clicks, etc.)
- Create ungrained cut, play from startOffset=44100
- Create grained cut (stretch=1.0), play from startOffset=44100
- Compare where each starts relative to file content
- This tells us definitively what domain k is in

### 2. Check Capture Page Code
- Review `twCapturingSource` and `CaptureRevalidator`
- Trace how offsets flow through capture pages
- Does capture convert between domains?

### 3. Verify twResampledSource Behavior
- Double-check: when twGrainSource reads from twResampledSource, what offsets does it expect?
- Are offsets converted correctly inside twResampledSource::read()?

---

## Code Locations to Verify

- `smaragd/main/src/scut.cpp:146` — Our offset scaling fix (rebuildReader)
- `smaragd/tw303a/src/twresampledsource.cc` — Does it handle offset conversion?
- `smaragd/tw303a/src/twcapturingsource.cc` — Capture page handling
- `smaragd/tw303a/src/capture_revalidator.cc` — Invalidation and reconstruction
