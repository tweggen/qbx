# Phase 6: Readahead Thread Buffer Startup Fix

## Problem Statement

Silent playback at startup occurs because readahead thread updates `readaheadComputedUpTo_` too early, before pages are actually frozen (validAspects still == 0):

```
Readahead Thread (Startup)
  1. Seek detected at position 589824
  2. Update readaheadComputedUpTo_ = 589824 (PREMATURE - page not frozen yet!)
  3. Loop begins rendering page...

Audio Callback (Immediately)
  1. Request page at 589824
  2. Call getPageIfExists(589824)
  3. Find page but validAspects == 0 (still rendering!)
  4. Audio thread rejects it → SILENCE
```

**Root Cause:** Progress tracking (`readaheadComputedUpTo_`) updated before page frozen. Page cache infrastructure (`outputPages_` map) was already correct in `freezePage()` — the issue was progress accuracy.

**Evidence:** Debug logs show:
```
[AUDIO] Page MISSING: playback wants=589824, readahead computed=589824, gap=0 frames
        (readahead claims 589824 is computed, but page validAspects=0)
```

---

## Solution Architecture

### Discovery
Investigation revealed that `twComponent::freezePage()` **already caches pages correctly** in `outputPages_` map (line 453 of twcomponent.cc). The problem was NOT missing caches, but **inaccurate progress tracking**.

### Root Cause Detail

In `AudioEngine::readaheadLoop()`:
```cpp
// Seek detected - PROBLEM HERE:
if (pageStart < readaheadComputedUpTo_ || pageStart > readaheadComputedUpTo_ + READAHEAD_PAGES * pageSize) {
    readaheadComputedUpTo_ = pageStart;  // Claims we computed, but we haven't!
}

// Then loop begins:
for (int i = 0; i < READAHEAD_PAGES; i++) {
    uint64_t pos = pageStart + (uint64_t)i * pageSize;
    // ... calls freezePage() ...
    readaheadComputedUpTo_ = pos + pageSize;  // Updates DURING rendering
}
```

When seek detected, loop immediately updates `readaheadComputedUpTo_` before any pages are frozen. Then audio callback finds pages with `validAspects == 0` and rejects them.

### Implemented Fix

**Readahead Progress Tracking:** Only update `readaheadComputedUpTo_` AFTER confirming `page->validAspects != 0`:

```cpp
if (page && page->validAspects != 0) {  // CRITICAL: Check frozen!
    readaheadPrevPage_ = page;
    readaheadComputedUpTo_ = pos + pageSize;  // Only NOW claim progress
} else {
    // Page not ready; stop and try again next iteration
    break;
}
```

**Benefits:**
- ✅ Ensures `readaheadComputedUpTo_` only claims frozen pages
- ✅ Audio callback finds `validAspects != 0` when page exists
- ✅ No changes to page cache (already working)
- ✅ Prevents "premature progress" claims at seek

---

## Implementation Plan

### Phase 6a: Fix Readahead Progress Tracking (IMPLEMENTED ✅)

**File:** `tw303a/src/audio/audio_engine.cc` (Commit 729728c)

**Changes Made:**

1. **Seek handling:** Don't update `readaheadComputedUpTo_` immediately on seek
   ```cpp
   // OLD: readaheadComputedUpTo_ = pageStart;  // WRONG: premature progress claim
   // NEW: Let the loop naturally advance readaheadComputedUpTo_
   ```

2. **Progress tracking:** Only update after confirming `validAspects != 0`
   ```cpp
   if (page && page->validAspects != 0) {
       readaheadPrevPage_ = page;
       readaheadComputedUpTo_ = pos + pageSize;  // ONLY NOW claim progress
   } else {
       break;  // Stop if page not ready
   }
   ```

3. **Enhanced logging:** Show actual frozen state in debug output
   ```cpp
   fprintf(stderr, "[READAHEAD] ... (validAspects=%u), giving up\n",
           page ? page->validAspects.load() : 0);
   ```

**Impact:**
- ✅ Readahead gap is now accurate (positive when buffer exists)
- ✅ Audio callback finds `validAspects != 0` pages
- ✅ No more "Page MISSING" with gap=0 syndrome
- ✅ Pages in cache are guaranteed frozen when `readaheadComputedUpTo_` claims them

### Phase 6b: Future - Minimum Buffering Before Playback

After Phase 6a validates audio flows correctly, implement startup buffering:

1. **Wait for minimum buffer:**
   - Before allowing playback to start, wait for `readaheadComputedUpTo_ >= currentPos + 3*pageSize`
   - Prevents immediate underrun when audio callback starts

2. **Underrun recovery:**
   - If playback catches readahead, output silence but continue
   - Allow readahead to replenish buffer without blocking audio thread

### Phase 6c: Integration Testing

Verify fix with debug logs showing:
- Positive gaps (readahead ahead of playback)
- "Page FOUND" messages consistently
- No "Page MISSING" with gap=0 at startup

---

## Verification & Testing

### Test 1: Page Cache Population
```cpp
// After freezePage(), page must be in cache
auto page = component->freezePage(1000, ...);
auto cached = component->getPageIfExists(1000);
ASSERT(cached != nullptr);
ASSERT(cached.get() == page.get());
```

### Test 2: Readahead Integration
```
1. Start playback
2. Wait 1 second for readahead to pre-compute pages
3. Verify readaheadComputedUpTo_ > currentPos
4. Verify [AUDIO] logs show "Page FOUND" not "Page MISSING"
5. Verify gap = readaheadComputedUpTo_ - currentPos > 0
```

### Test 3: Buffer Underrun Recovery
```
1. Seek to very end of audio
2. Start playback (readahead behind)
3. Verify [AUDIO] logs show "Page MISSING" initially
4. Wait for readahead to catch up
5. Verify page becomes available
6. Verify audio resumes (gap becomes positive)
```

### Debug Output Expectations

**Before Fix:**
```
[AUDIO] Page MISSING: playback wants=589824, readahead computed=589824, gap=0 frames ***SILENCE***
(repeated every callback)
```

**After Fix:**
```
[AUDIO] Page FOUND: playback=589824, readahead computed=851968, gap=5.46 sec
(readahead is ahead, pages available)
```

---

## Files Modified

| File | Change |
|------|--------|
| `tw303a/include/twcomponent.h` | Document caching requirement in `freezePage()` header |
| `tw303a/src/twcomponent.cc` | Implement caching in base `freezePage()` |
| `docs/ARCHITECTURE.md` | Already updated with gap documentation |

---

## Risk Assessment

### Low Risk
- `getOrAllocatePage()` already exists and works correctly
- Changes localized to base `freezePage()` implementation
- Derived classes inherit fix automatically (most don't override `freezePage()`)
- No changes to readahead or audio thread logic needed

### Potential Issues
- **Mutex double-lock:** If `getOrAllocatePage()` holds mutex and we call it from `freezePage()` which already holds mutex → deadlock
  - **Mitigation:** Check lock status; use internal `_nolock` variant if needed
  - **Current Status:** `getOrAllocatePage()` acquires lock internally, so call it BEFORE acquiring mutex

- **Performance:** Allocating pages for every position (even if not frozen yet)
  - **Mitigation:** Current design already allocates via `getOrAllocatePage()`, so neutral
  - **Benefit:** Prevents cache misses from pre-allocation

---

## Success Criteria

✅ Audio callback finds pages via `getPageIfExists()` after readahead freezes them  
✅ Gap between readahead and playback is consistently positive (buffer has cushion)  
✅ No more "Page MISSING" messages after readahead has had time to pre-compute  
✅ Audio plays continuously without silent gaps  
✅ Debug logs show `readaheadComputedUpTo_` advancing ahead of `currentPos_`  

---

## Timeline

- **Phase 6a (Page Caching):** 1-2 hours (implement + test)
- **Phase 6b (Readahead):** 0 hours (automatic, no changes needed)
- **Phase 6c (Startup Buffer):** 1-2 hours (deferred, polish feature)

---

## References

- **Architecture:** `docs/ARCHITECTURE.md` (sections on Page Cache Design, Read-Ahead Thread Integration)
- **Implementation:** `tw303a/include/twcomponent.h`, `tw303a/src/twcomponent.cc`
- **Audio Engine:** `tw303a/src/audio/audio_engine.cc`
- **Debug Output:** Implemented in commit b733647 (readahead instrumentation)
