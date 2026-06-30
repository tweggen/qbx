# Phase 2: twStreamingLatch Migration to freezePage()

**Date:** 2026-06-30  
**Status:** IN PROGRESS  
**Priority:** CRITICAL (hot path, affects UI responsiveness)

---

## Overview

Migrate `twStreamingLatch::copyData()` from calling raw-pointer `calcOutputTo()` to using the new `freezePage()` interface. This is the primary internal caller and represents the bridge between the legacy streaming latch system and the new page-based rendering.

---

## Current Architecture

### twStreamingLatch Call Chain

```
twLatchStreamingOutput::readStreamingData() [twlatch.cc:26]
  ↓ calls
twStreamingLatch::copyData() [twstreaminglatch.cc:53]
  ↓ line 204
getComponent().calcOutputTo(pBuffer+bufPos, maxFill, getIndex())  ← DEPRECATED
```

### Key Classes

- **twStreamingLatch** — buffering wrapper around a twComponent
  - Maintains ring buffer: `pBuffer`, `bufPos`, `bufSize`, `offset`
  - `copyData()` method handles streaming reads from ring buffer
  
- **twLatchStreamingOutput** — output interface
  - Wraps twStreamingLatch
  - `readStreamingData()` calls `copyData()` and updates offset
  - Used by components reading from latches (input plugs)

---

## Migration Strategy

### Option A: Wrap freezePage() Results (RECOMMENDED)

Replace line 204 in `twstreaminglatch.cc`:

```cpp
// OLD (line 204):
filled = getComponent().calcOutputTo( pBuffer + bufPos, maxFill, getIndex() );

// NEW:
auto page = getComponent().freezePage(
    currentPos_,              // snapshot position for state
    nullptr,                  // no input (source component)
    0,                        // no input offset
    maxFill,                  // length to fill
    sampleRate_,              // project sample rate
    previousPage_             // cached state from last page
);

if (page && page->samples.size() > 0) {
    length_t copyLen = std::min((size_t)maxFill, page->samples.size());
    memcpy(pBuffer + bufPos, page->samples.data(), copyLen * sizeof(sample_t));
    filled = copyLen;
    previousPage_ = page;     // cache for next freezePage() call
} else {
    filled = 0;
}
```

**Advantages:**
- Minimal caller disruption (copyData interface unchanged)
- Leverages page caching (avoids redundant renders)
- State snapshots work correctly (previousPage_ maintains internal state)
- Single point of change (one line → ~15 lines)

**Disadvantages:**
- Requires access to currentPos_ and sampleRate_ from twStreamingLatch
- Requires caching previousPage_ in twStreamingLatch
- Adds small overhead (page allocation vs raw buffer pull)

### Option B: Change Consumer Model (NOT RECOMMENDED for Phase 2)

Redesign all consumers of readStreamingData() to use freezePage() directly.
- Would affect ~15 call sites in components
- Higher risk, wider blast radius
- Deferred to Phase 4 (unification step)

---

## Implementation Plan

### Step 1: Add State to twStreamingLatch

**File:** `smaragd/tw303a/include/twcomponent.h` (lines 95-113)

Add to `twStreamingLatch` class:
```cpp
private:
    offset_t currentPos_;                              // Current playback position
    std::shared_ptr<twOutputPage> previousPage_;      // Cached page for state
    int sampleRate_;                                   // Project sample rate (from env)
```

### Step 2: Update copyData() Implementation

**File:** `smaragd/tw303a/src/twstreaminglatch.cc` (line 204)

Replace raw-pointer calcOutputTo() call with freezePage() wrapper (see Option A above).

Add initialization in constructor and reset in relevant methods.

### Step 3: Add Position Tracking

In `twLatchStreamingOutput::readStreamingData()` (twlatch.cc:26-35):
- Update currentPos_ after copying data
- This tracks the current playback position for the snapshot

### Step 4: Verify No Regression

- Build with -Wdeprecated-declarations
- Confirm twstreaminglatch.cc has zero deprecation warnings
- Run existing tests: playback, preview, render

---

## Risk Analysis

### High Risk (MITIGATE)

1. **State Snapshot Correctness**
   - freezePage() maintains component internal state via previousPage_
   - Must verify previousPage_ is properly cached and reset on seek
   - **Mitigation:** Add debug logging to verify state transitions

2. **Position Tracking During Seeks**
   - currentPos_ must sync with audio thread seek events
   - **Mitigation:** Coordinate with twComponent::seekTo() calls

3. **Buffer Alignment**
   - Ring buffer wraparound logic complex; don't break memcpy paths
   - **Mitigation:** Leave copyData() wraparound code unchanged, only replace the fill source

### Medium Risk (MONITOR)

4. **Sample Rate Mismatch**
   - freezePage() may resample; streaming latch expects native rate
   - **Mitigation:** Verify sampleRate_ is project rate, not device rate

5. **Page Size Mismatch**
   - page->samples.size() may not match maxFill request
   - **Mitigation:** Add bounds check (std::min above) and logging

---

## Testing Checklist

### Before Commit

- [ ] Compilation: zero errors, expected deprecation warnings only
- [ ] Unit test: twstreaminglatch_test (if exists)
- [ ] Integration: load project and preview waveform (GUI)
- [ ] Playback: play track from start, mid-project, and looped
- [ ] Seek: click timeline during playback
- [ ] Render: export WAV and verify audio integrity

### Post-Commit Verification

- [ ] CI passes (test suite)
- [ ] No audio dropout on waveform preview scrubbing
- [ ] No latency regression vs Phase 1 baseline

---

## Files to Modify

| File | Change | Lines |
|------|--------|-------|
| `tw303a/include/twcomponent.h` | Add state fields to twStreamingLatch | 95-113 |
| `tw303a/src/twstreaminglatch.cc` | Replace calcOutputTo call with freezePage wrapper | 204 + init |
| `tw303a/src/twlatch.cc` | Update position tracking in readStreamingData | 26-35 |

---

## Implementation Notes

1. **Constructor Initialization**
   - Set currentPos_ = 0 on creation
   - previousPage_ = nullptr initially
   - sampleRate_ from env.getRenderSRate()

2. **Seek Handling**
   - When twComponent::seekTo() called, reset previousPage_ = nullptr
   - Force fresh freezePage() on next read (no stale state)

3. **Loop/Cycle Handling**
   - currentPos_ wraps naturally (audio thread manages loop boundaries)
   - freezePage() handles wraparound via its own logic

4. **Error Handling**
   - If freezePage() returns empty page → filled = 0 (triggers CRITICAL error in original code)
   - Same behavior as calcOutputTo returning 0

---

## Timeline

- **Today:** Complete implementation + basic testing (2-3 hours)
- **Next:** Full integration test + UI verification (1 hour)
- **After:** Commit + document lessons for Phase 3

---

## Success Criteria

✅ Phase 2 Complete when:
1. twstreaminglatch.cc has zero [[deprecated]] warnings
2. All tests pass (98/100 → 100/100 or better)
3. No regression in UI responsiveness (preview, scrubbing, seek)
4. No audio dropout or artifacts during playback
5. Code review approved

---

**Next Phase:** Phase 3 — Remove raw-pointer calcOutputTo() from all 18 components
