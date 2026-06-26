# Render Silence Bug Investigation Guide

## Setup

**Diagnostic Build:** The codebase has been instrumented with comprehensive logging to trace the mixer path (Path A) during rendering. See `smaragd/tw303a/src/twtrackmix.cc` and `smaragd/main/src/strack.cpp`.

**Build Status:** ✅ Clean build on macOS
```
Binary: /Users/tweggen/coding/github/qbx/smaragd/build/bin/smaragd.app/Contents/MacOS/smaragd
```

## Reproduction Scenario (from plan 11)

**Problem:** Rendering timeline 4-12 seconds produces:
- First 4 seconds (timeline 4-8s): **SILENT** (should have audio)
- Last 4 seconds (timeline 8-12s): **HAS AUDIO** (correct)

**Container-backed cuts on STrack 2** with plainwave children at sample positions:
- 192000 (4 seconds @ 48kHz)
- 240000
- 288000
- 336000

## How to Reproduce

### Prerequisites
1. App is built with diagnostics enabled
2. Terminal ready to capture stderr output
3. Test project with container-backed cuts (or create from scratch)

### Steps

1. **Create test project:**
   - File → New Project
   - Add a track (Track 1)
   - Import sample (e.g., test_sawtooth.wav) at position 0
   - Add nested track (Track 2, with 2-3 plainwave cuts at different positions)
   - Create group cut wrapping Track 2
   - Place group cut on Track 1 starting at timeline position 4 seconds

2. **Run diagnostic render:**
   ```bash
   cd /Users/tweggen/coding/github/qbx
   # Redirect stderr to capture logs
   ./smaragd/build/bin/smaragd.app/Contents/MacOS/smaragd 2> render_debug.log &
   
   # Via UI: File → Render...
   # Set timeline range: Start=4s, End=12s
   # Format: WAV, output file
   # Click Render
   
   # Then: fg (bring to foreground when done)
   # Kill with Ctrl+C
   ```

3. **Examine logs:**
   ```bash
   grep "\[Mixer\]\|\[SCut\]\|\[STrack\]\|\[twTrackMix\]" render_debug.log > mixer_trace.log
   less mixer_trace.log
   ```

## Diagnostic Output Interpretation

### Expected logs for working render (Path B - renderObjectInto)

```
[SCut::buildCapture_] ENTER: capture_=0x...
[SCut::buildCapture_] PROCEEDING with capture build
[SCut::buildCapture_] DIAGNOSTIC: snap.startOffset=..., snap.cutDuration=..., container_dur=..., need=..., n=...
```

### Expected logs for mixer path (Path A - should see this for timeline 4-12s)

When rendering timeline 4-12s, we should see:
1. **twTrackMix::calcOutputTo called with correct range:**
   ```
   [twTrackMix::calcOutputTo] startInterval=192000, endInterval=576000, playLen=384000
   ```
   *(192000 = 4s @ 48kHz, 384000 = 8s, 576000 = 12s)*

2. **Children iterated and range-checked:**
   ```
   [Mixer] Child 1: startTime=0, checking range [192000, 576000)
   [Mixer] Child 1: SEEKING with startOffset=192000
   [Mixer] Child 1: doRead=384000
   [Mixer] Child 1: actuallyGot=384000 samples
   ```

3. **STrack::seekTo called:**
   ```
   [STrack::seekTo] Called with ofs=192000
   [STrack::seekTo] Seeking mixer 0 to 192000
   [twTrackMix::seekTo] Setting playOffset_=192000
   ```

## Key Questions to Answer from Logs

1. **Is STrack 2 even included in the mixer's render range?**
   - Look for `[Mixer] Child N: startTime=...` entries
   - If none appear, STrack 2 is being filtered out (bug!)

2. **Is the correct startOffset being calculated?**
   - For timeline 4-12s render starting at sample 192000
   - If child starts at 0, startOffset should be 192000
   - If startOffset=0, the mixer isn't seeking correctly

3. **Are children being sought to the right offset?**
   - Look for `[Mixer] Child N: SEEKING with startOffset=...`
   - Should show startOffset=192000 for children in STrack 2

4. **After seek, does the child produce audio?**
   - Look for `[Mixer] Child N: actuallyGot=...` 
   - If actuallyGot=0 or much less than doRead, child is producing silence after seeking

5. **Are we even reaching STrack 2's calcOutputTo?**
   - Nested mixer logs would show: `[twTrackMix::calcOutputTo]` for STrack 2's mixer
   - If absent, the link seek or getRootComponent failed

## Files to Check During Debug

- `smaragd/tw303a/src/twtrackmix.cc:67-131` — mixer iteration and seeking logic
- `smaragd/main/src/strack.cpp:95-103` — STrack::seekTo propagation
- `smaragd/tw303a/src/twtrackmix.cc:14-17` — twTrackMix::seekTo (playOffset_ update)
- `smaragd/main/src/scut.cpp:254-306` — buildCapture_() (compare Path B vs Path A)

## Hypothesis Testing Checklist

### If logs show STrack 2 children NOT being iterated:
- [ ] Check: Do range checks incorrectly filter STrack 2?
- [ ] Check: Does STrack 2 have valid startTime/duration?
- [ ] Check: Is track_.childLinks() returning all children?
- **Fix approach:** Debug range check logic in calcOutputTo

### If logs show STrack 2 children included but producing silence:
- [ ] Check: Is lk->seekTo(192000) actually called?
- [ ] Check: After seek, is child getRootComponent() valid?
- [ ] Check: Does getRootComponent()->calcOutputTo() return 0 samples?
- **Fix approach:** Debug seek propagation or child component state

### If logs show Path B (renderObjectInto) producing audio but Path A silent:
- [ ] Compare: renderObjectInto (line 230-243) vs twTrackMix iteration (line 80-118)
- [ ] Difference: renderObjectInto iterates ALL children; mixer uses range checks
- [ ] Difference: renderObjectInto doesn't seek; mixer seeks each child
- **Fix approach:** Unify the two paths or fix mixer's range check/seek logic

## Next Steps

1. **Run the diagnostic build with a test project and capture stderr**
2. **Filter logs for mixer activity:** `grep "\[Mixer\]" render_debug.log`
3. **Identify which hypothesis matches the logs**
4. **Add more targeted logging to the suspected code section**
5. **Implement the fix once root cause is confirmed**

---

**Commit reference:** c3ac0d6 (Diagnostics: Add comprehensive logging to trace render silence bug in mixer path)
