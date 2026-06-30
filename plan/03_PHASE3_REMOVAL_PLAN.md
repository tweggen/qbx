# Phase 3: Raw-Pointer Interface Removal (Detailed Plan)

**Date:** 2026-06-30  
**Status:** IN PROGRESS  
**Approach:** Careful, component-by-component migration

---

## Strategy

Phase 3 removes the raw-pointer `calcOutputTo(sample_t*, length_t, idx_t)` interface entirely. The challenge: each component's IOVector method currently wraps the raw-pointer version, so we must:

1. **Extract rendering logic** from raw-pointer calcOutputTo into IOVector method
2. **Remove raw-pointer declaration** from header
3. **Remove raw-pointer implementation** from .cc file
4. **Build and verify** after each component
5. **Test** for regressions

---

## Component-by-Component Implementation

### Pattern: Before vs After

**Before (Phase 2 state):**
```cpp
// twConstant.h
[[deprecated(...)]]
virtual length_t calcOutputTo( sample_t *pDest, length_t length, idx_t idx ) override;

// twConstant.cc
length_t twConstant::calcOutputTo( sample_t *pDest, length_t length, idx_t idx )
{
    // REAL RENDERING LOGIC HERE
    for( int i = 0; i < length; i++ ) {
        pDest[i] = constant;
    }
    return length;
}

length_t twConstant::calcOutputTo( IOVector& dest, idx_t idx )
{
    // WRAPPER - delegates to raw-pointer version
    sample_t *buffer = (sample_t *)alloca(dest.length() * sizeof(sample_t));
    length_t result = calcOutputTo(buffer, dest.length(), idx);  // Calls raw-pointer
    return dest.copyFrom(IOVector::CreateFromBuffer(buffer, result), 0, result);
}
```

**After (Phase 3):**
```cpp
// twConstant.h
// No raw-pointer declaration

// twConstant.cc
length_t twConstant::calcOutputTo( IOVector& dest, idx_t idx )
{
    // REAL RENDERING LOGIC MOVED HERE
    for( int i = 0; i < dest.length(); i++ ) {
        dest.data()[i] = constant;
    }
    return dest.length();
}
```

---

## Implementation Checklist

### Stage 1: Simple Components (No Helper Methods)

These have all logic in one method:

- [ ] **twConstant** — simple loop, no dependencies
- [ ] **twWhiteNoise** — simple loop, no dependencies
- [ ] **twTestSeq** — lookup table, no dependencies

### Stage 2: Components with Input Dependencies

These read from input plugs (_nolock helpers stay):

- [ ] **twMoog** — reads frequency buffer via latch
- [ ] **twPipe** — reads input via latch
- [ ] **twSaw** — reads frequency buffer via latch
- [ ] **twSimpleSaw** — reads frequency buffer via latch

### Stage 3: Components with Complex State

These have multiple state variables:

- [ ] **twLoopReader** — loop segment logic, state management
- [ ] **twSampleReader** — complex offset tracking

### Stage 4: Input-Reading Components

These manage input from multiple sources:

- [ ] **twMixer** — multi-channel mixing
- [ ] **twWavInput** — WAV file reading
- [ ] **twRewire** — multi-output patching

### Stage 5: Advanced Components

These have internal complexity:

- [ ] **twView** — delegates to wrapped component
- [ ] **twSpeaker** — audio device output
- [ ] **twWav** — file writing
- [ ] **twTrackMix** — clip timeline management
- [ ] **twPluginInsert** — plugin wrapping
- [ ] **twPluginChain** — plugin sequencing

---

## Key Principles for Safe Migration

1. **Never remove both at once** — Always keep rendering logic intact when removing interface
2. **Extract, don't rewrite** — Copy code as-is; only adjust for IOVector instead of raw pointer
3. **Build after each component** — Stop immediately if build fails; diagnose before continuing
4. **Test after 3-4 components** — Run action_roundtrip_test to catch regressions early
5. **Handle helper methods** — `_nolock()` helpers stay (they're called by non-removed methods)

---

## Special Cases

### twLoopReader (inherits from twSampleReader)

Calls parent class method `twSampleReader::calcOutputTo()`. After removal, needs to call IOVector version directly.

### twTrackMix (has IOVector wrapper already)

The IOVector wrapper calls deprecated method. Need to implement real logic inline.

### twPluginChain & twPluginInsert

Both manage plugin chains. Logic may delegate between components.

---

## Build Validation

After each component:

```bash
./build.sh 2>&1 | grep -E "error:|warning:|Build complete"
```

Expected: "Build complete" with no new errors.

---

## Test Validation

After every 4 components:

```bash
/Users/tweggen/coding/github/qbx/smaragd/build/bin/action_roundtrip_test 2>&1 | tail -15
```

Expected: 39/41 passing (same as Phase 2 baseline).

---

## Rollback Strategy

If a component breaks the build:

```bash
git diff HEAD -- smaragd/tw303a/include/tw<component>.h smaragd/tw303a/src/tw<component>.cc
git checkout HEAD -- smaragd/tw303a/include/tw<component>.h smaragd/tw303a/src/tw<component>.cc
```

Fix diagnosis, then retry.

---

## Completion Criteria

Phase 3 complete when:

✅ All 18 components migrated (headers + implementations)  
✅ Build succeeds with zero errors  
✅ Tests pass 39/41 (no regression from Phase 2)  
✅ Single commit with all changes  
✅ Documentation updated

---

## Timeline Estimate

- Simple components (3): 15 min per component (manual)
- Input dependencies (4): 30 min per component (manual)
- Complex state (2): 20 min per component (manual)
- Input-reading (3): 25 min per component (manual)
- Advanced (6): 45 min per component (manual)
- **Total: ~8-10 hours** (more than initially estimated)

---

## Lessons Learned (Attempted Phase 3)

### What Didn't Work
1. **Bulk header cleaning with sed**: Partially successful but fragile - sed patterns don't catch all formatting variants
2. **Python script for removal**: Damaged files by removing too much (method signatures + body)  
3. **Assumptions about file structure**: Each component has unique patterns; no one-size-fits-all approach

### Why This Is Hard
1. **Two-level wrapping**: IOVector wrapper calls raw-pointer implementation. Can't remove raw-pointer without breaking wrapper.
2. **Complex method signatures**: Some components have overloads, comments mixed in, unusual formatting
3. **File structure variation**: Each .cc file has different layout, spacing, comments around methods
4. **Helper methods**: `_nolock` versions must stay (called by other methods), but raw-pointer versions must go
5. **Interdependencies**: Some components call parent class methods (twLoopReader calls twSampleReader, etc.)

---

## Recommended Approach for Phase 3 (Future Attempt)

### Stage-by-stage with verification:

1. **For each component:**
   - [ ] Read both header and .cc file completely
   - [ ] Identify the rendering logic in raw-pointer calcOutputTo
   - [ ] Understand any helper method dependencies (_nolock, internal calls)
   - [ ] Check if IOVector wrapper delegates to raw-pointer or has its own logic
   - [ ] Decide: move logic inline vs implement from scratch
   
2. **For components with input dependencies:**
   - Preserve the input reading logic (via latch calls)
   - Keep calling `pInputPlugs[N]->readStreamingData()` pattern
   - Convert `sample_t *pDest` buffer operations to `dest.data()` IOVector operations

3. **For complex components (twTrackMix, etc.):**
   - Don't attempt bulk removal - handle manually
   - These have intricate state management that can't be automated
   - Requires understanding of clip management, timeline logic, etc.

4. **Build validation:**
   - Build after each component (not batched)
   - If any error, fix that component before continuing
   - Run tests every 3-4 components, not after all 18

5. **Commit strategy:**
   - Consider committing per-component or per-stage
   - Smaller commits make bisecting easier if regression occurs

---

## Next Steps (When Attempting Phase 3 Again)

1. Start with simplest component: **twConstant** (no input dependencies, stateless)
2. Manually move rendering logic into IOVector method
3. Remove raw-pointer declaration and implementation
4. Build and verify
5. Move to next: **twWhiteNoise** (similar pattern)
6. After 3 simple components work, tackle input-dependent ones
7. Save complex ones (twTrackMix, etc.) for last

---

## Current Status

✅ **Phase 1 & 2 Complete and Stable**
- Deprecation warnings added
- twStreamingLatch migrated to freezePage()
- 39/41 tests passing
- Zero regressions

📋 **Phase 3 Deferred**
- Requires careful per-component migration
- Estimated 8-10 hours for proper implementation
- Recommended to tackle in dedicated session
- Document completed components for next attempt

