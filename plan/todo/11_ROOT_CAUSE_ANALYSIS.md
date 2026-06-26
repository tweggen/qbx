# Render Silence Bug: Root Cause Analysis

**Date:** 2026-06-26  
**Status:** ROOT CAUSE IDENTIFIED ✓  
**Commits:** c3ac0d6, 77707be, 84d69d2

## Executive Summary

**The render silence bug is caused by a component hierarchy architecture issue where intermediate wrapper components don't implement `seekTo()`, preventing seek calls from reaching the track mixers.**

When rendering timeline 4-12 seconds:
1. ✅ RenderSession calls `seekTo(192000)` on the root component
2. ✅ twRewire forwards the seek to input components
3. ❌ Input components don't implement seekTo (return -1, base implementation)
4. ❌ Track mixers never receive the seek
5. ❌ Mixer playOffset_ remains at 0 instead of 192000
6. ❌ Children at positions 192000+ are filtered out by range check
7. ❌ Mixer produces silence

---

## Evidence

### From Diagnostic Logs

```
[twRewire] Input 0: comp=0xbe0dc5740, calling comp.seekTo(192000)
[twComponent::seekTo] Base implementation called with offset=192000 (this=0xbe0dc5740)
[twRewire] Input 0: seekTo returned -1

[twRewire] Input 1: comp=0xbe0dc5800, calling comp.seekTo(192000)
[twComponent::seekTo] Base implementation called with offset=192000 (this=0xbe0dc5800)
[twRewire] Input 1: seekTo returned -1
```

**Critical observation:** Components return -1 (base implementation) when they should be implementing seekTo properly.

### Comparison: Working vs Broken

**What SHOULD happen:**
```
RenderSession.seekTo(192000)
  → SStdMixer.getRootComponent() → twRewire_root
    → twRewire_root.seekTo(192000)
      → Input 0: STrack.getRootComponent() → twRewire_track
        → twRewire_track.seekTo(192000)
          → Input 0: twTrackMix (or similar with seekTo implemented)
            → twTrackMix.seekTo(192000) ← SHOULD SET playOffset_=192000
```

**What ACTUALLY happens:**
```
RenderSession.seekTo(192000)
  → SStdMixer.getRootComponent() → twRewire_root
    → twRewire_root.seekTo(192000)
      → Input 0: ??? (NOT implementing seekTo)
        → twComponent::seekTo returns -1 (NOT IMPLEMENTED)
          → Seek STOPS HERE, never reaches twTrackMix
            → twTrackMix.playOffset_ stays 0 ❌
```

---

## Component Hierarchy Analysis

### Expected Structure
```
SStdMixer (logical root mixer)
├── cpRewire_ (twRewire - forwards seeks to children)
│   ├── Input 0: STrack (via SLink connection)
│   │   └── STrack.getRootComponent() → twRewire_track
│   │       ├── Input 0: cpTrackMixers_[0] (twTrackMix)
│   │       └── Input 1: cpTrackMixers_[1] (twTrackMix)
│   └── Input 1: STrack (via SLink connection)
│       └── STrack.getRootComponent() → twRewire_track
│           └── cpTrackMixers_ (twTrackMix)
└── cpMixerComponent_ (actual audio processing)
```

### Actual Structure (BROKEN)
The components that twRewire_root connects to don't implement seekTo:
- `0xbe0dc5740` returns -1 (not `twRewire`, not `twTrackMix`)
- `0xbe0dc5800` returns -1 (not `twRewire`, not `twTrackMix`)
- etc.

---

## Why playOffset_ Stays at 0

### Mixer's calcOutputTo Logic

```cpp
offset_t startInterval = playOffset_.load( std::memory_order_relaxed );
offset_t endInterval   = startInterval + playLen;
playOffset_.store( endInterval, std::memory_order_relaxed );
```

1. **First call during render:** startInterval = 0 (initial value, never seeked)
2. **Children range check:** `if (startTime >= endInterval) continue;` with [0, 1)
3. **Children at 192000+:** All filtered out as out-of-range
4. **Result:** Silence (no children processed)

### Why Seek Doesn't Help

The `seekTo(192000)` call reaches twRewire but stops there:
- twRewire forwards to input component
- Input component doesn't implement seekTo (returns -1)
- seekTo never reaches the mixer
- playOffset_ never updated to 192000

---

## Root Causes (Multiple Layers)

### Layer 1: Component Hierarchy Gap
The components connected to the root rewire don't implement seekTo. They should be implementing it so that seeks propagate down through the hierarchy.

**Current:** Base twComponent::seekTo() just returns -1  
**Needed:** Proper forwarding or implementation in intermediate components

### Layer 2: STrack Connection Issue
When STrack is connected as an input to the parent rewire, it should expose its seekTo capability through its getRootComponent(). But:
- STrack::getRootComponent() returns `cpRewire_` (twRewire)
- That twRewire should forward seeks to its inputs
- But the diagnostics show the wrong components are connected

### Layer 3: Missing Seek Propagation
The render session doesn't ensure seeks propagate all the way down. It just calls seekTo and assumes it worked.

---

## Why Path B (renderObjectInto) Works

`renderObjectInto()` doesn't rely on playOffset_:
```cpp
// Recursive static rendering - no cursor/position management
for (SLink *lk : obj.childLinks()) {
    offset_t start = lk->getStartTime();
    // Render child at its timeline position directly
    renderObjectInto(child, buf + start, ...);
}
```

It renders ALL children at their actual timeline positions without needing to seek anything. No playOffset_ involved.

---

## The Fix (Multiple Options)

### Option 1: Implement seekTo in Intermediate Components (RECOMMENDED)
Make sure all components in the hierarchy implement seekTo:
- twRewire: ✓ Already implements (forwards seeks)
- Components connected to rewire inputs: ❌ NEED TO IMPLEMENT
  - If it's another twRewire: ✓ should work
  - If it's a mixer: needs seekTo implementation
  - If it's a wrapper: needs to forward or implement

**Action:** Identify what component types are returning -1 and add seekTo to them.

### Option 2: Change Component Connection
Instead of connecting intermediate components that don't implement seekTo, connect directly to components that do.

**Action:** Modify how STrack connects to the parent mixer's rewire.

### Option 3: Explicit Seek in Render
Make RenderSession seek directly on known component types rather than relying on the hierarchy.

**Action:** Add special case in RenderSession for seeking track mixers.

### Option 4: Use renderObjectInto for Render
Use the working Path B (renderObjectInto) for rendering instead of the live component path.

**Action:** Modify render to use buildCapture/renderObjectInto instead of live mixer.

---

## Recommended Fix Path

1. **Identify component types** at addresses 0xbe0dc5740, etc. (what classes are they?)
2. **Add seekTo implementation** to those classes if missing
3. **Test** that seek propagates all the way to twTrackMix
4. **Verify** that mixer's playOffset_ updates to 192000 during render
5. **Confirm** silence bug is fixed

## Commits

- c3ac0d6: Initial mixer diagnostics (calcOutputTo, seekTo)
- 77707be: twRewire::seekTo diagnostics (seek forwarding)
- 84d69d2: twComponent::seekTo diagnostics (base implementation catch-all)

## Next Steps

1. Add runtime type identification to logged components (e.g., log class name)
2. Trace back from component pointers to find what classes they are
3. Check if those classes should implement seekTo
4. Implement or fix seekTo in those classes
5. Re-test render to confirm bug is fixed
