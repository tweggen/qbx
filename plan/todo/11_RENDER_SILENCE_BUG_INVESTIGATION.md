# Container-Backed Cut Rendering: Silent First Half Bug

**Problem Statement:**
When rendering timeline 4-12 seconds, the output is:
- First 4 seconds (timeline 4-8s): **SILENT** — should have STrack 2's content
- Last 4 seconds (timeline 8-12s): **HAS AUDIO** — correctly renders container-backed cut

**Key Observations:**
1. Playback works correctly (container-backed cut produces audio)
2. Rendering via `renderObjectInto()` (inside buildCapture_) produces audio
3. Rendering via mixer's direct path produces silence for STrack 2
4. seekTo() diagnostics show children being sought from `off=0, 1, 2...` not `off=192000+`

**Hypothesis:**
Two different rendering paths exist:
- **Path A (Mixer):** `mixer → STrack 2 → children` = SILENT
- **Path B (Container Cut):** `renderObjectInto(STrack 2)` = AUDIO

Path A is not seeking STrack 2 to the correct position during render, while Path B works correctly.

---

## Investigation Strategy

### Phase 1: Confirm the Two Paths Exist

**Objective:** Verify that STrack 2 is being rendered via both paths.

**Tests:**
1. Add logging to `twTrackMix::calcOutputTo()` in the mixer to see:
   - Which children are being iterated
   - What `startOffset` is calculated for each
   - Whether STrack 2 is included in the render range

2. Look for in the render logs:
   - Lines showing mixer iterating children
   - Lines showing STrack 2 being included/excluded
   - Lines showing children at positions 192000, 240000, 288000, 336000

3. Confirm via seekTo() diagnostics:
   - Search for `snap.startOffset=428925` (plainwave cuts in STrack 2)
   - Note the `off` values: should start at 192000 if mixer seeks correctly, or 0 if not

---

### Phase 2: Trace Path A (Mixer → STrack 2)

**Objective:** Understand why mixer's direct rendering produces silence.

**Trace Points:**
```
mixer.calcOutputTo()
  ↓ [checks: is STrack 2 in range 192000-576000?]
  ↓ [calculates: startOffset = 192000 - 0 = 192000]
  ↓ lk->seekTo(192000) [DOES THIS HAPPEN?]
  ↓ lk->getRootComponent() [returns STrack 2's component]
  ↓ component.calcOutputTo() [STrack 2's mixer]
    ↓ [STrack 2 loads playOffset_: should be 192000 if seeked]
    ↓ [iterates children, checks range 192000-...]
```

**Key Questions:**
1. Is mixer even including STrack 2? (startTime=0, duration=384000, so should be in range [192000, 576000])
2. Is mixer calling `lk->seekTo(192000)` on STrack 2?
3. After seek, is STrack 2's `playOffset_` actually 192000?
4. Are STrack 2's children at positions 192000+ found by the range check?

**Diagnostics to Add:**
```cpp
// In twTrackMix::calcOutputTo() - mixer level
fprintf(stderr, "[Mixer::calcOutputTo] startInterval=%ld, endInterval=%ld\n", startInterval, endInterval);
for(SLink *lk : track_.childLinks()) {
    fprintf(stderr, "[Mixer] Child startTime=%ld, duration=%ld\n", startTime, duration);
    fprintf(stderr, "[Mixer] Calculated startOffset=%ld\n", startOffset);
}

// In STrack::seekTo()
fprintf(stderr, "[STrack::seekTo] Called with off=%ld, will call twTrackMix::seekTo\n", ofs);

// In twTrackMix::seekTo()
fprintf(stderr, "[TrackMix::seekTo] Setting playOffset_=%ld\n", newOffset);

// In twTrackMix::calcOutputTo() - STrack 2 level
fprintf(stderr, "[STrack2::calcOutputTo] playOffset_=%ld, startInterval=%ld, endInterval=%ld\n", 
        startInterval, startInterval, startInterval + playLen);
```

---

### Phase 3: Trace Path B (renderObjectInto)

**Objective:** Understand why renderObjectInto produces correct audio.

**Trace Points:**
```
buildCapture_() for container-backed cut
  ↓ [reads snap: startOffset=192000, cutDuration=192000]
  ↓ c.seekTo(0) [seek container to start]
  ↓ renderObjectInto(c, buf, n) [render full container]
    ↓ [iterates STrack 2's children]
    ↓ [for each child, calls renderObjectInto recursively]
    ↓ [child renders to buffer at its timeline position]
  ↓ [capture created with full rendered content]
  ↓ reader seeks to startOffset=192000
  ↓ reader reads 192000 samples → AUDIO
```

**Why it works:**
- `renderObjectInto()` renders ALL children at their timeline positions
- Children at 192000, 240000, 288000, 336000 are included
- Buffer has full content, reader reads the correct portion

---

### Phase 4: Identify the Divergence

**Objective:** Find where the two paths produce different results.

**Compare:**
| Aspect | Path A (Mixer) | Path B (renderObjectInto) |
|--------|---|---|
| Initial seek | `seekTo(192000)` on STrack 2? | `seekTo(0)` on container |
| Content iteration | Live `twTrackMix::calcOutputTo()` | Static `renderObjectInto()` loop |
| Child inclusion | Range check: [192000, 576000) | Direct iteration, no range skip |
| Audio result | SILENT | AUDIO |

**Hypothesis:** Path A's range check might be skipping children, or playOffset_ not being set correctly.

---

### Phase 5: Root Cause Determination

**If Path A is skipping children:**
- Fix: Ensure mixer range check includes STrack 2's children at 192000+
- Fix: Verify seekTo propagation works during render

**If Path A's playOffset_ is wrong:**
- Fix: Ensure `twTrackMix::seekTo(192000)` is called and sets playOffset_ correctly
- Fix: Debug playOffset_ management during render startup

**If it's a conceptual issue:**
- Fix: Path A and Path B may need to be made consistent
- Consider: Should mixer use static iteration like renderObjectInto?

---

## Testing & Verification

### Before Fixes:
1. Run render with Phase 2 diagnostics enabled
2. Capture full logs showing Path A's behavior
3. Identify exactly where/why silence occurs

### After Fixes:
1. Render 4-12 seconds
2. Verify output file has audio throughout (8 seconds, not 4)
3. Compare waveform with playback result
4. Ensure both first 4s and last 4s have audio

---

## Files to Instrument

- `tw303a/src/twtrackmix.cc` — mixer's calcOutputTo() 
- `main/src/strack.cpp` — STrack::seekTo()
- `main/src/scut.cpp` — existing diagnostics (buildCapture_, seekTo)
- `tw303a/src/sstdmixer.cc` — root mixer iteration

---

## Expected Outcome

By the end of Phase 4, we should understand:
1. **Whether** STrack 2 is even being attempted to render via Path A
2. **Why** Path B (renderObjectInto) produces audio while Path A doesn't
3. **How to fix** the discrepancy so both paths work correctly

This investigation respects the principle: **if nothing changed, the output shouldn't change either.** If rendering produces different output than playback, there's a logic error in one of the paths—not a caching issue.
