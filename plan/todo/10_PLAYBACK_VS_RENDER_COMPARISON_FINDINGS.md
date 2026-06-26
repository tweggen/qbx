# Phase 1: Playback Path Findings

**Status:** Analysis complete. Key code paths traced and documented.

## Executive Summary

Both playback and rendering use the SAME code path for getting audio:
1. `twTrackMix::calcOutputTo()` iterates through child links
2. For each child, calls `lk->seekTo(startOffset)`
3. Calls `lk->getRootComponent()` which triggers `SCut::ensureReader()`
4. For container-backed cuts, `ensureReader()` calls `buildCapture_()`
5. Then returns a reader that outputs audio

**Key Difference:**
- Playback: continuously advances `playOffset_` frame by frame
- Rendering: seeks once at start, then advances `playOffset_`

## Playback Path: Initialization & First Audio Pull

### 1. Starting Playback
```cpp
// SApplication::setPlaying(true) at playback start
// Speaker's callback pulls frames continuously
```

### 2. Speaker Callback → AudioEngine → twTrackMix
```
twSpeaker::calcOutputTo()
  ↓
AudioEngine::pullStereoFrame()
  ↓
synthOutput_->linkOutput(0)  [gets mixer's left channel]
  ↓
twTrackMix::calcOutputTo()
```

### 3. twTrackMix::calcOutputTo() - KEY ALGORITHM
File: `smaragd/tw303a/src/twtrackmix.cc` lines 67-131

```cpp
length_t twTrackMix::calcOutputTo(sample_t *buffer, length_t playLen, idx_t outChannel)
{
    offset_t startInterval = playOffset_.load(...);           // Line 71: Load current position
    offset_t endInterval = startInterval + playLen;           // Line 72: Calculate range
    playOffset_.store(endInterval, ...);                       // Line 73: Advance playOffset
    
    for(SLink *lk : track_.childLinks()) {                    // Line 80: Iterate children
        offset_t startTime = lk->getStartTime();              // Line 82: Child's timeline position
        if(startTime >= endInterval) continue;                // Line 83: Past our range?
        offset_t endTime = startTime + child.getDuration();   // Line 84-86: Child's end time
        if(startInterval >= endTime) continue;                // Line 87: Before our range?
        
        // Calculate how much of THIS child to read
        offset_t startOffset;                                 // Line 90: Offset into child
        if(startTime < startInterval) {
            startOffset = startInterval - startTime;          // Line 92: We're starting mid-child
        } else {
            startOffset = 0;                                  // Line 95: We're before the child
        }
        
        lk->seekTo(startOffset);                              // Line 103: **SEEK CHILD TO OFFSET**
        twComponent &cp = lk->getRootComponent();             // Line 104: **TRIGGERS ensureReader()**
        
        // Read audio
        length_t actuallyGot = cp.calcOutputTo(readBuffer+destOffset, doRead, outChannel);
        // Mix into output
    }
}
```

**Critical Values During Playback:**
- `startInterval`: Current playback position in track timeline
- `startOffset`: Offset into child (0 if child starts after playback position)
- Both values advance continuously as frames are pulled

### 4. SCut::seekTo() - APPLIES WINDOW PARAMETERS
File: `smaragd/main/src/scut.cpp` lines 469-480

```cpp
int SCut::seekTo(offset_t off)
{
    ensureReader();
    SCutSnapshot snap = getSnapshot();
    
    // off = startOffset from twTrackMix (offset into this cut)
    // snap.startOffset = this cut's "slip" offset
    // snap.reader.looping = whether reader is a loop reader
    
    if(snap.reader.reader) {
        // APPLIES OFFSET: startOffset + snap.startOffset
        return snap.reader.reader->seekTo(snap.reader.looping ? off : off + snap.startOffset);
    }
    return content_->getSObject().seekTo(off + snap.startOffset);
}
```

**Key Point:** The offset parameter from `twTrackMix` (where to start in the child) is ADDED to the cut's own `startOffset` (where in the content to start reading).

### 5. SCut::getRootComponent() - TRIGGERS CAPTURE BUILD
File: `smaragd/main/src/scut.cpp` lines 440-449

```cpp
twComponent &SCut::getRootComponent()
{
    ensureReader();  // <-- Line 442: BUILDS CAPTURE FOR CONTAINERS
    SCutSnapshot snap = getSnapshot();
    if(snap.reader.reader) return *snap.reader.reader;
    return content_->getRootComponent();  // Fallback to content
}
```

### 6. SCut::ensureReader() - BUILDS CAPTURE
File: `smaragd/main/src/scut.cpp` lines 95-149

For container-backed cuts (no random source):
```cpp
if(!rs) {  // Line 102: No random source = container-backed
    buildCapture_();  // Line 103: BUILD THE CAPTURE BUFFER
    if(capture_) {
        rs = capture_.get();  // Use capture as the source
    }
}
```

Then builds the reader:
```cpp
// Line 118-126: Create reader chain (loop reader or plain reader)
if(snap.loopLength > 0 && snap.loopLength < snap.cutDuration) {
    twLoopReader *lr = new twLoopReader(env, *view, snap.startOffset, snap.loopLength);
    newReader = lr;
} else {
    newReader = view->acquireReader(env);
}
```

### 7. SCut::buildCapture_() - RENDERS CONTAINER TO BUFFER
File: `smaragd/main/src/scut.cpp` lines 252-287

```cpp
void SCut::buildCapture_()
{
    // Calculate buffer size
    SCutSnapshot snap = getSnapshot();
    length_t need = snap.startOffset + snap.cutDuration;
    length_t dur = container.getDuration();
    length_t n = dur > need ? dur : need;  // Line 264: max(duration, startOffset+cutDuration)
    
    // Render container to buffer
    c.seekTo(0);  // Line 275: Seek container to start
    std::vector<sample_t> buf((size_t)n, 0.0f);
    renderObjectInto(c, buf.data(), n, env, env.getSRate(), 0);  // Line 277: Render
    
    // Create reader from captured buffer
    capture_ = std::make_shared<twCapturingSource>(std::move(buf), n, 1, env.getSRate());
}
```

**Buffer Size Calculation:**
- If container has children at positions [0, 100, 200, ..., 400], duration = 400
- If cut has startOffset=100, cutDuration=200:
  - `need = 100 + 200 = 300`
  - `dur = 400`
  - `n = max(400, 300) = 400` → Render 400 samples
  - Reader will seek to position 100 and read 200 samples

### 8. renderObjectInto() - RECURSIVE CONTAINER RENDERING
File: `smaragd/main/src/scut.cpp` lines 182-247

**For a SCut with container content:**
```cpp
if(SCut *cut = dynamic_cast<SCut*>(&obj)) {
    SObject &content = cut->getContent();
    offset_t off = cut->getStartOffset();  // Line 199: Cut's startOffset
    
    if(!contentRs) {  // If content is a container
        // RECURSIVE: Render the container to a temp buffer
        length_t clen = content.getDuration();
        renderObjectInto(content, buf.data(), clen, env, rate, depth+1);  // Line 208
        // ...
    }
    
    // Then read from the cut's offset
    view->read(off, dest, len, 0);  // Line 221: Read from startOffset
}
```

**For a Container:**
```cpp
for(SLink *lk : obj.childLinks()) {  // Line 229: Iterate children
    offset_t start = lk->getStartTime();  // Line 232: Child's timeline position
    
    // Render child to temp buffer
    renderObjectInto(child, tmp.data(), n, env, rate, depth+1);  // Line 239
    
    // Place in output at child's position
    for(length_t i = 0; i < n; ++i) 
        dest[start + i] += tmp[i];  // Line 240: Sum at timeline position
}
```

## Playback: Snapshot of Key Values

When a container-backed cut is encountered during playback:

| Value | Example | Notes |
|-------|---------|-------|
| `twTrackMix::playOffset_` | 10000 | Current timeline position in track |
| `lk->getStartTime()` | 5000 | Child cut's timeline start position |
| `startOffset` passed to `seekTo()` | 5000 | `playOffset_ - startTime` (offset into child) |
| `snap.startOffset` | 100 | Cut's own "slip" offset parameter |
| `snap.cutDuration` | 200 | Cut's timeline duration |
| `buildCapture_()` calculates `n` | 400 | `max(container_dur=400, startOffset+cutDuration=300)` |
| Reader seeks to | 100 | `startOffset (5000) + snap.startOffset (100)` |
| Reader reads | 200 | `snap.cutDuration` |

Wait, there's an inconsistency here. Let me recalculate:
- `startOffset` from twTrackMix = 5000 (offset into the child)
- `snap.startOffset` = 100 (cut's slip offset)
- When seeking: `snap.reader.reader->seekTo(off + snap.startOffset)` = seekTo(5000 + 100) = 5100

But the capture is only 400 samples! So seeking to position 5100 is way out of bounds.

This doesn't make sense. Let me re-read the code...

AH! I see the issue now. The `startOffset` returned from `twTrackMix::calcOutputTo()` is NOT an absolute position - it's a RELATIVE offset within the child!

So if the child (the cut) is at timeline position 5000, and playOffset is 5100, then:
- `startOffset = 5100 - 5000 = 100` (offset into the cut, not absolute)

So the seek is to `startOffset (100) + snap.startOffset (0)` = 100 in the cut.

But wait, what if `snap.startOffset = 200`? Then we're seeking to 300 in the cut. If the cut's content is only 400 samples total, and we seek to 300 and read 100, we get the LAST 100 samples of the content. That's correct!

OK so the logic is:
1. `startOffset` from twTrackMix = relative offset within the child
2. `snap.startOffset` = additional offset within the cut's content (the "slip")
3. Sum them to get the actual position to seek

And `buildCapture_()` renders enough to cover `snap.startOffset + snap.cutDuration`.

## Diagnosis: Buffer Size Issue

**Hypothesis:** The issue might be in how `n` is calculated.

Current code:
```cpp
length_t need = snap.startOffset + snap.cutDuration;
length_t dur = container.getDuration();
length_t n = dur > need ? dur : need;
```

This renders up to `max(container_duration, startOffset+cutDuration)`.

**Potential Problem:** 
If `snap.startOffset` is roughly 50% of the container duration, and we calculate `n = max(dur, startOffset+cutDuration)`, then:
- Example: dur=400, startOffset=200, cutDuration=200
- need = 200 + 200 = 400
- n = max(400, 400) = 400 → Render all 400
- Reader seeks to position 200 and reads 200 samples
- Gets the SECOND HALF (200-400)
- First HALF (0-200) is not read!

**Why would this happen?**
If the container's children are positioned at timelines 0-400, they'll all be rendered to buffer positions 0-400. Then when we read from position 200, we get the second half of all children - which happens to be in the timeline range 200-400.

This would explain the "first half empty, second half has audio" symptom!

## Diagnostics Added

✓ **`buildCapture_()`** logs:
```
[SCut::buildCapture_] DIAGNOSTIC: snap.startOffset=XXX, snap.cutDuration=YYY, container_dur=ZZZ, need=NNN, n=MMM
[SCut::buildCapture_] Built capture: MMM samples
```

✓ **`seekTo()`** logs:
```
[SCut::seekTo] off=XXX, snap.startOffset=YYY, looping=true/false, final_seekPos=ZZZ
```

## Test Steps

To diagnose the rendering issue:

1. Open Smaragd with a project containing a container-backed cut (a SCut with a STrack as content)
2. Go to File → Render → Select output format and file
3. Check the terminal output for [SCut::buildCapture_] and [SCut::seekTo] diagnostics
4. Look for:
   - What `snap.startOffset` value is used when building capture?
   - Is it 0, or is it a large value?
   - What buffer size `n` is calculated?
   - What seek positions are used?

**Key Question:** If `snap.startOffset` is approximately 50% of container duration, that would explain the "second half only" symptom.

## Potential Root Cause

Based on code analysis, the issue is likely in `buildCapture_()` line 264:

```cpp
length_t n = dur > need ? dur : need;  // where need = startOffset + cutDuration
```

**Scenario that causes "second half empty":**
- Container duration = 400 samples
- snap.startOffset = 200 (somehow)
- snap.cutDuration = 200
- need = 200 + 200 = 400
- n = max(400, 400) = 400
- Render all 400 samples
- Reader seeks to position 200 and reads 200
- Gets samples [200, 400] = the SECOND HALF
- First half [0, 200] is not read

**Why would `snap.startOffset = 200`?**
This could happen if:
1. User set the cut's slip offset (startOffset_) to 200
2. OR there's a bug in how startOffset is being calculated/propagated
3. OR there's confusion between two different offset parameters

## Next Phase (Phase 2): Render Path Tracing

Will capture the same diagnostic output during rendering and compare values with playback.

