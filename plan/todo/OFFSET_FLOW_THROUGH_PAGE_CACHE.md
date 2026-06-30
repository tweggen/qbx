# Offset Flow Through Page Cache System - The Real Issue

## The Problem We've Been Missing

We were analyzing offsets in `rebuildReader()`, but that's not where the playback actually happens for grained cuts!

**The rendering flow is:**
```
Playback
  → SCut::getCapture(Playback)
  → CaptureRevalidator::recomputePlayback()
  → Depending on cut type, reads audio via page cache
```

The page cache system (Freezing Wires architecture) renders components into pages, and SCut reads FROM these pages.

---

## How Page Cache Works

### Page Structure
- Each page covers time range `[startPos..startPos+PAGE_SIZE)`
- Pages are ~256 kB = ~0.68 seconds @ 48 kHz stereo
- Rendering fills pages sequentially

### For Sample-Backed Cuts (SPlainWave)
When `recomputePlayback()` is called:
1. Get the SPlainWave object
2. Call its preview/playback rendering
3. SPlainWave owns a `twWavInput` component
4. twWavInput renders into the page cache

---

## The Offset Bug in Page Cache Context

### Setup: Grained 44.1 kHz file in 48 kHz project

```
File: 44.1 kHz, 88200 samples
Project: 48 kHz  
SCut: startOffset = k, grain.stretch = 2.0
```

### What Should Happen

When rendering the SCut's playback:

```cpp
// In CaptureRevalidator::recomputePlayback()
void recomputePlayback(SCut* cut, CapturePageData& page) {
    offset_t cutStartOffset = cut->getStartOffset();  // k
    
    // For a grained sample cut:
    // 1. The content source should prepare for reading from offset k
    // 2. After grain stretch, the audio extends to k + (duration * stretch)
    
    // If grain is applied via twGrainSource in rebuildReader():
    //   - reader is positioned at k * stretch (in stretched buffer domain)
    //   - twGrainSource::read() pulls from pre-materialized stretched audio
    //
    // But here's the problem: the page cache may not align with the reader!
}
```

### The Hidden Issue

There are TWO offset domains at play:

1. **SCut offset domain** (used by page cache):
   - `cutStartOffset = k` (whatever domain it's in)
   - Used when calling content's playback methods

2. **Reader offset domain** (used by twSampleReader):
   - Position in the sample stream
   - After grain: multiplied by stretch
   - May be in different domain than SCut's k

**If these don't align, playback reads wrong position!**

---

## Detailed Trace: How Offset Flows Through Page Cache for Grained Cut

### Phase 1: Build Time (rebuildReader)

```cpp
// scut.cpp:63 - rebuildReader()
twRandomSource *rs = content_->getSObject().getRandomSource();
// rs = twResampledSource @ 48 kHz (for SPlainWave)

if( newGrain ) {
    newGrain = std::make_shared<twGrainSource>(*rs, snap.grainParams);
    // twGrainSource pre-materializes stretched audio from twResampledSource
    // It reads the ENTIRE source: rs->read(0, buffer, rs->length(), ...)
    
    adjustedStartOffset = (offset_t)llround((double)snap.startOffset * snap.grainParams.stretch);
    // Line 146: We scale k by stretch
}

newReader = std::shared_ptr<twSampleReader>(view->acquireReader(env, adjustedStartOffset));
// Creates reader at position k*stretch
```

**At this point:**
- twGrainSource has pre-materialized buffer (entire stretched audio)
- twSampleReader is positioned at k*stretch
- This reader is stored in currentReader_

### Phase 2: Playback Time (getCapture → Playback page)

```cpp
// scut.cpp:1077 - getCapture()
auto page = getCapture(Playback);

// scut.cpp:1127 - recomputePlayback() [hypothetical, checking naming]
// This triggers CaptureRevalidator::recomputePlayback(cut, page)

// In CaptureRevalidator (capture_revalidator.cc):
void CaptureRevalidator::recomputePlayback(SCut *cut, CapturePageData &page) {
    // For sample-backed cut:
    SObject& content = cut->getContent();  // SPlainWave
    
    // How does SPlainWave know where to start reading?
    // It uses cut->getStartOffset() = k
    
    // But the reader chain was built with k*stretch!
    // ← DOMAIN MISMATCH!
}
```

### The Critical Question

When `CaptureRevalidator` renders a sample-backed grained cut:

**Does it:**
A) Use the pre-built reader chain (with k*stretch offset)?
   - Reader was built in rebuildReader() with that offset
   - Page cache rendering should use it
   
B) Call the content object's render methods (with k offset)?
   - Content object might not know about grain stretching
   - Render would use k directly
   - ✗ WRONG: offset not scaled!

---

## The Real Bug Location

The bug is likely **NOT** in rebuildReader() (we fixed that).

The bug is probably in **how the page cache rendering actually uses the reader chain**.

### Hypothesis: Page Cache Ignores Pre-Built Reader

If `CaptureRevalidator::recomputePlayback()` does:

```cpp
void recomputePlayback(SCut *cut, CapturePageData &page) {
    SObject& content = cut->getContent();
    
    // Directly calls content rendering (SPlainWave)
    // WITHOUT using the pre-built reader chain!
    
    offset_t offset = cut->getStartOffset();  // k, not k*stretch!
    content.getRootComponent()->calcOutputTo(..., offset, ...);
    // ✗ WRONG: passes k directly, not k*stretch!
}
```

This would explain why:
- Grain was applied (stretch exists in grainParams)
- But offset is wrong (using k not k*stretch)
- The fix to rebuildReader didn't help (it's not even used!)

---

## What Actually Gets Called?

To find the bug, we need to answer:

1. **When playback renders a sample-backed grained cut, what code path is used?**
   - Does it use `currentReader_` (built with k*stretch)?
   - Or does it call content.render with k?

2. **For container-backed cuts, how are offsets handled?**
   - These use capture pages differently
   - May have different offset handling

3. **How does grain stretching interact with page-aligned rendering?**
   - Pages are fixed size (~0.68s)
   - Grain stretches time
   - Are pages stretched? Or offset calculation?

---

## Files to Investigate

- `smaragd/tw303a/src/capture_revalidator.cc` — Where/how is playback actually rendered?
- `smaragd/main/src/scut.cpp` — recomputePlayback() method (if it exists)
- `smaragd/tw303a/src/twcapturingsource.cc` — How twCapturingSource handles offsets
- `smaragd/tw303a/src/tw_output_page.h` — Page structure and offset handling

---

## Summary of the Mystery

The page cache system renders components into frozen pages. But for a grained cut:

- **Pre-built reader chain** knows about grain (offset multiplied by stretch)
- **Page cache rendering** might not use that reader chain at all
- **Result**: Offset passed to content is wrong domain/scale

This is why preview (which might use a different path) works correctly—it might use the reader chain differently than playback.
