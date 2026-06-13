# Critical Render Position Bug: render1.wav vs render2.wav

## The Symptom

Two consecutive renders with **identical parameters** produced **completely different output:**

**Render Parameters (both):**
- RangeStart: 96000 samples (2.0 seconds)
- RangeEnd: 192000 samples (4.0 seconds)
- Duration: 96000 samples (2.0 seconds)

**Render 1 output:**
- Started at sawtooth value: -32766 (offset +2 from -32768)
- Position: Near sample 0 ❌ **WRONG**
- Expected: sample 96000

**Render 2 output:**
- Started at sawtooth value: -8768 (offset +24000 from -32768)  
- Position: Exactly sample 96000 ✓ **CORRECT**

The values differ by ~96000 samples—exactly the time range start position!

---

## Root Cause Analysis

### The Synth Position Tracking Issue

Both renders called the same code sequence:
```cpp
synthOutput_->seekTo(startOffsetSamples_);  // seekTo(96000)
twLatchOutput *synthOutputPlug = synthOutput_->linkOutput(0);
resampler_.process(synthOutputPlug, ...);
```

But produced different outputs.

### Hypothesis: Latch Offset Not Synchronized After Seek

The synth uses a **twLatch** to stream output. The latch has an `offset_t offset` field that tracks the current position. When you call `linkOutput(0)`, it creates an output plug initialized with the latch's current offset:

```cpp
twLatchOutput(twLatch & latch)
    : parentLatch(latch) { offset = latch.getOffset(); }
```

**The problem:** After calling `seekTo()` on the component, the latch's offset field may NOT be automatically updated to reflect the new position.

### Why render1 Failed and render2 Worked

**Render 1 timeline:**
1. RenderSession created fresh, resampler pristine
2. seekTo(96000) called on synth component
3. Latch offset field possibly not updated (still at old position or 0)
4. linkOutput(0) creates plug with stale offset
5. Resampler pulls from wrong position → renders from near sample 0

**Render 2 timeline:**
1. RenderSession REUSED (not recreated) 
2. Resampler still has leftover state from render 1
3. seekTo(96000) called again
4. By coincidence or state carryover, latch offset was already closer to correct position
5. linkOutput(0) creates plug with correct offset
6. Resampler pulls from correct position → renders from sample 96000 ✓

### Why Consecutive Renders Differ

The RenderSession is created once and reused for all renders:

```cpp
void SApplication::startRender(const audio::RenderParams &params) {
    if (!renderSession_) {
        renderSession_ = std::make_unique<audio::RenderSession>();  // Created once
    }
    // ...
    renderSession_->start(synthOutput, params, ...);  // Reused for all renders
}
```

This means:
- Leftover state from render 1 affects render 2
- The synth component's position state carries forward
- The latch's offset tracking may be inconsistent between renders

---

## Fix Strategies

### Option 1: Force RenderSession Recreation
```cpp
renderSession_.reset();  // Destroy and recreate for each render
renderSession_ = std::make_unique<audio::RenderSession>();
```
Pros: Clean state, no leftover effects
Cons: Small overhead per render

### Option 2: Explicitly Sync Latch Offset After Seek
Ensure the latch's offset field is updated to match the seek position:
```cpp
synthOutput_->seekTo(startOffsetSamples_);
// Force latch to acknowledge the new position somehow
```
Cons: Requires understanding latch's internal offset semantics

### Option 3: Delete and Recreate Output Plug Per Render
```cpp
// Don't reuse the same output plug connection
// Create a fresh one each time to force proper initialization
```
Cons: May have performance implications

---

## Evidence

The test_sawtooth.wav structure provides perfect evidence:
- Sawtooth: -32768 to +32767 (65536 distinct values)
- Each value held for 4 samples
- Each ramp: 262144 samples = 5.461333 seconds

**Render1 analysis:**
- First value: -32766 (very beginning, should be -8768 at sample 96000)
- Last value: -8769 (part of first ramp)
- Pattern: Perfect 4-sample repetition, but starting from wrong position

**Render2 analysis:**  
- First value: -8768 ✓ (correct for sample 96000)
- Last value: 15231 ✓ (correct for sample 192000)
- Pattern: Perfect 4-sample repetition, from correct position

The 96000-sample discrepancy (render1 starts at ~0, render2 starts at 96000) is not a coincidence—it exactly matches the render range start.

---

## Recommendations

1. **Immediate:** Test whether recreating RenderSession per render fixes the issue
2. **Investigation:** Examine twLatch::seekTo() behavior and offset field handling  
3. **Long-term:** Ensure explicit synchronization between component seek and latch offset
4. **Testing:** Always test consecutive renders with different time ranges to catch state carryover bugs
