# DSP Component Chain Bug Fixes (Track → Speaker)

## Context

Review of the signal chain `twTrackMix → twPluginChain → cpRewire_ (STrack) → twMixer → cpRewire_ (SStdMixer) → twSpeaker` revealed several bugs ranging from crash-level to memory leaks to silent audio incorrectness.

---

## Chain Overview

```
twTrackMix[i] → twPluginChain[i] → cpRewire_ (STrack) → twMixer[i] (SStdMixer) → cpRewire_ (SStdMixer) → twSpeaker
```

Each bus `i` has its own `twTrackMix` and `twPluginChain`. The STrack `cpRewire_` merges all buses and exposes them to the parent mixer.

---

## Bugs to Fix (Priority Order)

### BUG 1 — setNBusses grow path crashes on second call [CRITICAL]

**File:** `smaragd/main/src/strack.cpp:235-259`

On first call (from constructor, `cpPluginChains_` is NULL) `oldNBusses` is forced to 0 — works fine. On any subsequent `setNBusses(N)` call where N > current:
1. Lines 237-240 delete and null all existing chains
2. Lines 247-250 copy the nulled pointers into the new array
3. Lines 253-257 only create new chains for indices `[oldNBusses..nBusses-1]`

Result: indices 0..oldNBusses-1 are NULL → line 275 (`cpPluginChains_[i]->setInput(...)`) crashes.

**Fix:** In the grow path, preserve existing chain pointers instead of deleting them first. Only delete and recreate chains for removed buses (shrink) or create new ones for added buses (grow). Pattern:
```cpp
// Grow: copy existing (valid) chains, create new ones for new indices only
for( int i = 0; i < oldNBusses; ++i )
    newChains[i] = cpPluginChains_[i];   // keep existing — do NOT delete
for( int i = oldNBusses; i < nBusses; ++i ) {
    newChains[i] = new twPluginChain( env, 1 );
    newChains[i]->init();
}
```

---

### BUG 2 — twPluginChain double-initialization [MEDIUM: memory leak / UB]

**File:** `smaragd/tw303a/src/twpluginchain.cc:5-10`, `smaragd/main/src/strack.cpp:256`

Constructor calls `allocPlugs()` + `createOutputLatches()` directly. Then `init()` (called from strack.cpp:256) calls them again via `twComponent::init()`, leaking the first allocation. The `new[]` block from the first `createOutputLatches()` is overwritten by the `calloc` block from the second `allocPlugs()` and never freed.

**Fix:** Remove the `allocPlugs()` and `createOutputLatches()` calls from the `twPluginChain` constructor body. Rely on `init()` as all other components do:
```cpp
twPluginChain::twPluginChain( tw303aEnvironment &env, idx_t nBusses )
    : twComponent( env ), nBusses_( nBusses )
{
    // allocPlugs() and createOutputLatches() called by init()
}
```

---

### BUG 3 — Speaker only wired to input 0; right channel always silent [HIGH: audio correctness]

**File:** `smaragd/main/src/sapplication.cpp:50-62`

`rewireSpeaker()` only calls `getSpeaker()->setInput(0, src)`. `twSpeaker::getNInputs()` returns 2 (left + right). When input 1 is NULL the render callback copies left to right — output is always mono.

**Fix:** Wire both speaker inputs, guarded by whether the root has 2 outputs:
```cpp
twComponent &root = currentProject_->getRootComponent()->getRootComponent();
getSpeaker()->setInput( 0, root.linkOutput( 0 ) );
if( root.getNOutputs() > 1 )
    getSpeaker()->setInput( 1, root.linkOutput( 1 ) );
```
Note: SStdMixer's internal rewire currently has only 1 bus. Full stereo requires the mixer's bus count to match. This fix is safe to land now and will activate automatically when the mixer supports 2 buses.

---

### BUG 4 — ~STrack leaks cpRewire_ and all cpTrackMixers_[i] [MEDIUM: memory leak]

**File:** `smaragd/main/src/strack.cpp:318-337`

Destructor handles `cpPluginChains_` correctly but never deletes `cpRewire_`, any `cpTrackMixers_[i]`, or the `cpTrackMixers_` array itself.

**Fix:**
```cpp
if( cpTrackMixers_ ) {
    for( int i = 0; i < nBusses_; ++i )
        delete cpTrackMixers_[i];
    ::free( cpTrackMixers_ );
}
delete cpRewire_;
```

---

### BUG 5 — ~SStdMixer leaks all DSP objects [MEDIUM: memory leak, known FIXME]

**File:** `smaragd/main/src/sstdmixer.cpp:373-376`

Known `// FIXME: Free cpMixer.` — neither `cpRewire_`, the `cpMixers_[]` array, nor any individual `twMixer` objects are deleted.

**Fix:**
```cpp
SStdMixer::~SStdMixer()
{
    if( cpMixers_ ) {
        for( int i = 0; i < nBusses_; ++i )
            delete cpMixers_[i];
        ::free( cpMixers_ );
    }
    delete cpRewire_;
}
```

---

### BUG 6 — setNBusses shrink path is a stub; silent UAF on render [HIGH: potential crash]

**File:** `smaragd/main/src/strack.cpp:207-213`

The shrink branch is empty (`// FIXME: Write this.`). The plugin chain block below runs unconditionally, deleting all chains while `cpRewire_` still holds stale `twLatchOutput*` pointers to them. Next render = use-after-free.

**Fix (minimal):** Guard with an early return until the shrink path is properly implemented:
```cpp
if( nBusses < oldNBusses ) {
    // Shrink not yet implemented; assert in debug, silently refuse in release
    Q_ASSERT_X( false, "STrack::setNBusses", "bus count shrink not supported" );
    return;
}
```

---

## Files to Modify

| File | Bugs |
|------|------|
| `smaragd/tw303a/src/twpluginchain.cc` | BUG 2 |
| `smaragd/main/src/strack.cpp` | BUG 1, BUG 4, BUG 6 |
| `smaragd/main/src/sapplication.cpp` | BUG 3 |
| `smaragd/main/src/sstdmixer.cpp` | BUG 5 |

## Verification

1. Build with AddressSanitizer — double-free and UAF from BUGs 1 & 2 should vanish
2. Load a project, play audio, confirm no crash
3. Add a track, remove it, re-add it (exercises `setNBusses`)
4. Confirm stereo output with a panned signal after BUG 3 fix
