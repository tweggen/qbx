# Buffer Crash Fixes: Audio Resampling Path

**Date:** 2026-06-30  
**Status:** ✅ COMPLETE  
**Commits:**
- `a08e586` — Fix: Buffer overflow and underrun in audio resampling path
- `7b6ee1c` — Cleanup: Remove verbose buffer diagnostics, add silence detection

---

## Problem Statement

Application crashed immediately on audio playback when resampling was active (device rate ≠ project rate). Specifically:
- **Symptom:** EXC_BAD_ACCESS in `__bzero` during CoreAudio callback
- **Trigger:** Play audio with SCut→SPlainWave unmuted, device at 44.1kHz, project at 48kHz
- **Error:** Address `0x170cc4000` (CoreAudio buffer, likely unmapped or protected memory)

## Root Causes

### 1. Buffer Allocation Mismatch (Primary)

When resampling 48kHz → 44.1kHz (downsampling):
- Output requested: 512 frames at 44.1kHz
- Input needed: `ceil(512 * 48000/44100)` = 558 frames at 48kHz
- Math: `invRatio = 1.0884` (inputRate / outputRate)

**Bug:** Code allocated local buffers with `nFrames` (512) instead of `inFramesNeeded` (558):
```cpp
// WRONG:
std::vector<float> bufL(nFrames), bufR(nFrames);  // Only 512 floats
for (length_t i = 0; i < inFramesNeeded; ++i) {   // Try to access 558!
    bufL[i] = ...  // Buffer overflow
}
```

**Fix:** Allocate correct size:
```cpp
// CORRECT:
std::vector<float> bufL(inFramesNeeded), bufR(inFramesNeeded);  // 558 floats
for (length_t i = 0; i < inFramesNeeded; ++i) {
    bufL[i] = ...  // Safe
}
```

### 2. Frozen Page Boundary Underrun (Secondary)

Frozen pages are 65536 frames each. When `pageFrameOffset_` reached the end of a page:
- Code returned `false` (underrun) instead of advancing to the next page
- This caused silence and audio dropout at page boundaries

**Bug:**
```cpp
if (pageFrameOffset_ >= currentFrozenPage_->validFrames) {
    return false;  // Dead end!
}
```

**Fix:** Advance to next page:
```cpp
if (pageFrameOffset_ >= currentFrozenPage_->validFrames) {
    // Advance to next page
    pos = (currentPageStartPos_ / twOutputPage::FRAME_CAPACITY + 1) 
        * twOutputPage::FRAME_CAPACITY;
    updateFrozenPage(pos);
    
    // Try again with new page
    if (!currentFrozenPage_ || pageFrameOffset_ >= currentFrozenPage_->validFrames) {
        return false;  // Genuinely at end
    }
}
```

### 3. Forward Declaration Type Mismatch

`twOutputPage` was forward-declared as `class` but defined as `struct`:
```cpp
class twOutputPage;  // WRONG: it's a struct!
```

**Fix:**
```cpp
struct twOutputPage;  // CORRECT
```

---

## Changes Made

### audio_engine.cc

1. **Buffer allocation (line 59):** Changed from `nFrames` to `inFramesNeeded`
2. **Page boundary handling (lines 196-218):** Added logic to advance to next page instead of returning underrun
3. **Diagnostics:** Added silence detection (logs only when all output is zeros, every 100 callbacks)

### twspeaker.cc

1. **Callback cleanup:** Removed verbose frame-by-frame diagnostics to reduce console noise

### audio_engine.h

1. **Forward declaration (line 13):** Changed `class twOutputPage` to `struct twOutputPage`

---

## Testing

**Before Fix:**
- ❌ Crash (EXC_BAD_ACCESS) immediately on playback with resampling
- ❌ No audio output

**After Fix:**
- ✅ No crash on playback start
- ✅ Audio streams stably through page boundaries
- ✅ Audio is audible (tested on built-in speakers and Bluetooth headset)
- ✅ Resampling works correctly (512 output frames from 558 input frames)

---

## Implicit Assumptions (Now Validated)

The buffer handling refactoring revealed and fixed these implicit assumptions:

1. **Caller's buffer size contract:** Functions assume buffers are large enough for requested frames
   - `pullBlock(outL, outR, nFrames)` assumes caller provides buffers holding ≥ nFrames
   - With resampling, input buffer must hold ≥ ceil(nFrames * invRatio) frames

2. **Page continuity:** Frozen pages transition seamlessly; reaching page end is not a fatal underrun
   - Audio stream must advance across page boundaries automatically
   - Each page is 65536 frames; seek logic must handle multi-page ranges

3. **Component graph stability:** `freezePage()` can safely render pages concurrently
   - Previous work on mutex patterns ensures no data races during page rendering
   - Removed lock from rendering path to prevent deadlock

---

## Related Issues Resolved

- **Silence on playback:** Was masking buffer crash; fixed by correcting page transitions
- **Device-specific crashes:** Tested on multiple devices (built-in speakers, Bluetooth); all stable
- **Resampling accuracy:** Linear interpolation now works on correct sample counts

---

## Future Work

1. **IOVector refactoring** (proposal 13_IO_VECTOR_SAFE_BUFFERS.md): Type-safe buffer wrappers to prevent similar issues
2. **Resampler quality:** Upgrade from linear to polyphase/windowed-sinc (proposal 04)
3. **Threading audit:** Review mutex patterns in component graph (separate from this buffer fix)

---

## Lessons Learned

1. **Rate conversion math is easy to get wrong.** Always calculate invRatio and verify frame counts.
2. **Buffer size contracts must be explicit.** Document caller assumptions clearly.
3. **Page boundaries need explicit handling.** Don't assume single-page buffers in streaming scenarios.
4. **Device-agnostic testing is critical.** Bug manifests differently (or not at all) on different audio devices.
