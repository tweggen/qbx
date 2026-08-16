# Audio Signal Chain Architecture

## Overview

Smaragd's audio signal chain connects the SObject model hierarchy (UI + user data) to the twComponent DSP graph (real-time audio processing) via a series of wiring calls. This document traces the complete chain from project root through to hardware output.

---

## Part 1: SObject Model Hierarchy

The SObject tree is the persistent model. All objects live as QObject children of `SProject`, linked via `SLink` relationships:

```
SProject (root container)
  ├── Metadata: sample rate, tempo, loop markers
  │
  └── getRootComponent() → SStdMixer (the active mixer)
        │
        ├── SLink → STrack (track 0)
        │     ├── Metadata: gain dB, mute, solo, pan
        │     │
        │     ├── SLink → SCut (clip at timeline position T)
        │     │     ├── Metadata: start position, duration, loop mode
        │     │     │
        │     │     └── SLink → SPlainWave (content reference)
        │     │           └── Metadata: file path, offset, length
        │     │
        │     ├── SLink → SCut (clip at timeline position T')
        │     │     └── ... (same structure)
        │     │
        │     └── SPluginChain (effect inserts for this track)
        │           └── Metadata: bypass state, plugin list
        │
        ├── SLink → STrack (track 1)
        │     └── ... (repeat)
        │
        └── SLink → STrack (track N)
```

---

## Part 2: twComponent DSP Graph

Every SObject exposes a `twComponent` subgraph via `getRootComponent()`. These components are wired together **separately** from the Qt parent/child hierarchy using `setInput(idx, latchOutput)` and `linkOutput(idx)` calls.

### Build Order

1. **SProject::setRootComponent(SStdMixer\*)**
   - SStdMixer creates its twComponent subgraph
2. **SStdMixer::setChannels(n)** — n from `SProject::channels()`
   - ONE `twMixer` (the bus sum) and ONE `twRewire` (the graph ROOT), each **n
     channels wide**. A bus is not a channel: there is one bus, and the channel
     dimension lives inside the page (proposal 36 B4). Before B4 the mixer ran
     `bus < 1` and every track's second channel was built and then dropped.
   - Wires: `twMixer->linkOutput(0)` → `twRewire->setInput(0, ...)`
3. **SStdMixer::reconnectTracksToMixer()** — for each track
   - Gets track's root component: `STrack::getRootComponent()` → twRewire
   - Wires: `twRewire[track]->linkOutput(0)` → `twMixer->setInput(track, ...)`
   - `twMixer`'s INPUTS are tracks; its CHANNELS are channels. It sums channel
     *c* of every input into channel *c*, clamping a narrower track's last
     channel up (§4.4, "mono plays on every channel").
4. **STrack::setChannels(n)** — per-track infrastructure, same n
   - Creates ONE `twTrackMix`, ONE `twPluginChain`, ONE `twRewire`, all n wide
   - Wires: `twTrackMix` → `twPluginChain` → `twRewire->setInput(0, ...)`
   - Called again on `SProject::channelsChanged`; a later call only re-widths,
     creating and destroying nothing (so a shrink is ordinary)
5. **STrack::trackChildWasAdded(SLink)** — when clip added
   - Calls `twTrackMix->insertClip(startTime, duration, getComponentFn)`
   - `getComponentFn` is a lambda capturing the SLink, invoked dynamically
6. **twTrackMix::insertClip()** — adds clip to timeline
   - Creates `twView` (stable wrapper)
   - Stores in `clips_` vector with timeline position
7. **SCut::getRootComponent()** — returns the reader
   - Builds `twSampleReader` (or `twLoopReader`, or `twGrainSource` chain)
   - Positioned at the clip's offset within SPlainWave
8. **SPlainWave::getRootComponent()** — returns the file decoder
   - Returns `twWavInput` (WAV file reader)

### Complete Signal Flow Diagram

```
Audio Callback (CoreAudio/WASAPI/ALSA @ device rate)
    ↓
twSpeaker::pullBlock()
    ├─ Calls audio::AudioEngine::pullBlock()
    │   └─ AudioEngine manages page-based rendering at project rate
    │
    └─ Resample project rate → device rate (if needed)


AudioEngine::pullStereoFrameFrozen()
    ├─ Load current timeline position (atomic)
    ├─ Call updateFrozenPage(pos)
    │   └─ Call synthOutput_->getPageIfExists(pageStartPos)
    │
    └─ Extract sample from currentFrozenPage_


synthOutput_ = SStdMixer::getRootComponent() = twRewire (root mixer output)
    ├─ freezePage(startPos, ..., previousPage)
    │   ├─ FreezeContext installed for cycle detection
    │   ├─ Call renderFrames() → calcOutputTo(IOVector&)
    │   └─ Cache result under mutex
    │
    └─ calcOutputTo() [line 52 in twrewire.cc]
        └─ Iterate input plugs:
            └─ readStreamingData(pInputPlugs[i])
                └─ copyData() on upstream latch


twMixer[bus 0] (sum of all tracks on bus 0)
    ├─ calcOutputTo(IOVector&) [line 67 in twmixer.cc]
    │   └─ for each track input:
    │       └─ readStreamingData(track_rewire_output)
    │           └─ copyData() on track's output latch
    │
    └─ Acquires frozen page from track's root twRewire


STrack[track_i]::getRootComponent() = twRewire (track output)
    ├─ freezePage() [inherited, uses base calcOutputTo]
    ├─ calcOutputTo() → readStreamingData()
    │   └─ Triggers upstream plugin chain freeze
    │
    └─ Reads from twPluginChain[bus 0]


twPluginChain[bus 0] (effect inserts)
    ├─ freezePage() [inherited, uses base calcOutputTo]
    ├─ calcOutputTo() → readStreamingData()
    │   └─ Triggers upstream twTrackMix freeze
    │
    └─ Chains: plugin[0] → plugin[1] → ... → output


twTrackMix[bus 0] (timeline clip mixer for track)
    ├─ freezePage() [CUSTOM OVERRIDE at line 231 in twtrackmix.cc]
    │   ├─ Lock acquired
    │   ├─ Check cache for page at startPos
    │   └─ Call freezePage_nolock() if not cached
    │       ├─ FreezeContext installed
    │       ├─ For each clip overlapping [startPos, startPos+64K):
    │       │   ├─ Compute clip-relative position: childPos = startPos - clip.startTime
    │       │   ├─ Call clip.view->freezePage(childPos, ..., clip.previousPage)
    │       │   │   └─ twView forwards to dynamic SCut::getRootComponent()
    │       │   │       └─ Typically: twSampleReader
    │       │   └─ Mix returned page into accumulator (IOVector::mixFrom)
    │       ├─ Apply track gain & mute
    │       └─ Capture internal state (none for twTrackMix; no-op)
    │
    └─ Returns frozen page with mixed clip outputs


twView[clip_j] (stable wrapper)
    ├─ getComponentFn callback captures SLink
    ├─ Every method forwards to dynamically-resolved component
    ├─ freezePage() [CUSTOM OVERRIDE at line 44 in twview.cc]
    │   └─ Calls comp->freezePage(...) where comp = getComponentFn()
    │
    └─ Enables SCut's component to change (e.g., WAV swap) without invalidating pointer


SCut::getRootComponent() = twSampleReader (or twLoopReader, twGrainSource wrapper)
    ├─ Created in SCut::rebuildReader()
    ├─ Position state: start offset in SPlainWave, current play position
    │
    ├─ freezePage() [inherited, uses base calcOutputTo]
    │   ├─ FreezeContext installed
    │   ├─ restoreInternalState(previousPage) — restores play position
    │   ├─ renderFrames() → calcOutputTo(IOVector&)
    │   └─ captureInternalState(page) — saves play position for next page
    │
    └─ calcOutputTo() → readStreamingData()
        └─ Reads from upstream: SPlainWave's twWavInput


SPlainWave::getRootComponent() = twWavInput (WAV file reader)
    ├─ Position: offset in file
    │
    ├─ freezePage() [inherited, uses base calcOutputTo]
    │   ├─ restoreInternalState(previousPage) — restores file position
    │   ├─ renderFrames() → calcOutputTo(IOVector&)
    │   └─ captureInternalState(page) — saves file position
    │
    └─ calcOutputTo()
        └─ Read samples from twSampleSource (in-RAM WAV buffer)


Page Cache & State Continuity
    ├─ Each component caches frozen pages by startPosition
    ├─ Page contains 65,536 samples @ project rate + internal state snapshot
    ├─ Next page reads: previousPage.internalState → restoreInternalState()
    │   ├─ Maintains reader position across page boundaries
    │   ├─ Maintains filter state (poles, delay lines) — IF captured
    │   └─ Maintains oscillator phase — IF captured
    │
    └─ Seamless audio: no resets, no pops
```

---

## Part 3: Key Wiring Call Sites

| Step | Component | File | Line(s) | Code Fragment |
|------|-----------|------|---------|---|
| 1 | SStdMixer created | main/src/smainwindow.cpp | ~217 | `fileNew()` → `SProject::setRootComponent(new SStdMixer(...))` |
| 2 | Speaker wired to root | main/src/sapplication.cpp | 51–63 | `rewireSpeaker()` → `getSpeaker()->setInput(0, root.linkOutput(0))` |
| 3a | SStdMixer twMixer created | main/objects/mixer/src/sstdmixer.cpp | — | `setNBusses(1)` → ONE `twMixer`, `setChannels(n)` → n channels wide |
| 3b | SStdMixer twRewire created | main/objects/mixer/src/sstdmixer.cpp | — | `new twRewire(env)`, `setNPlugs(1)`, `setChannels(n)` |
| 3c | Mixer→Rewire wiring | main/objects/mixer/src/sstdmixer.cpp | — | `cpRewire_->setInput(0, mix->linkOutput(0))` |
| 4 | Tracks → Mixer wiring | main/objects/mixer/src/sstdmixer.cpp | — | `reconnectTracksToMixer()` → `mix->setInput(trackIndex, track_rewire.linkOutput(0))` |
| 5a | STrack twTrackMix created | main/objects/track/src/strack.cpp | — | `setChannels()` → ONE `twTrackMix(env)`, n channels wide |
| 5b | STrack twPluginChain created | main/objects/track/src/strack.cpp | — | `new twPluginChain(env, n)` — ONE, one port each way |
| 5c | STrack twRewire created | main/objects/track/src/strack.cpp | — | `new twRewire(env)`, `setNPlugs(1)` |
| 5d | TrackMix→Chain→Rewire wiring | main/objects/track/src/strack.cpp | — | `cpDspChain_->setInput(0, cpTrackMix_->linkOutput(0))` etc. |
| 6 | Clips inserted into twTrackMix | main/objects/track/src/strack.cpp | — | `trackChildWasAdded()` → `cpTrackMix_->insertClip(startTime, duration, getComponentFn)` |
| 7 | twView created for clip | tw303a/src/twtrackmix.cc | 88–106 | `insertClip()` → `twView *view = new twView(env, getComponentFn)` |
| 8 | SCut reader built | main/src/scut.cpp | 469–481 | `getRootComponent()` → builds or returns `twSampleReader`, `twLoopReader`, etc. |
| 9 | SPlainWave reader created | main/src/splainwave.cpp | 65–68 | `getRootComponent()` → returns `cpWave_` (twWavInput) |

---

## Part 4: freezePage Participation

### Summary Table

| Component | File | freezePage() Override? | Mechanism | State Snapshot? |
|-----------|------|---|---|---|
| **twTrackMix** | twtrackmix.cc | **YES** (line 231) | Iterates clips, calls `clip.view->freezePage()`, mixes outputs, applies track gain | Minimal (no audio state) |
| **twView** | twview.cc | **YES** (line 44) | Forwards to dynamically-resolved component via callback | None (pure proxy) |
| **twRewire** | twrewire.cc | No (base class) | `calcOutputTo()` → `readStreamingData()` on single input plug | None (stateless router) |
| **twMixer** | twmixer.cc | No (base class) | `calcOutputTo()` → `readStreamingData()` × N inputs, accumulate with per-input gain | None (stateless adder) |
| **twPluginChain** | twpluginchain.cc | No (base class) | `calcOutputTo()` → iterates plugins, chains via `twStreamingLatch` | None (chains stateless plugins) |
| **twSampleReader** | twsamplereader.cc | No (base class) | `calcOutputTo()` reads via latch from `twSampleSource`; **has `captureInternalState()` / `restoreInternalState()`** | **YES** — play position |
| **twWavInput** | twwavinput.cc | No (base class) | `calcOutputTo()` reads samples from `twSampleSource` | **YES** — file offset |
| **twMoog** | twmoog.cc | No (base class) | `calcOutputTo()` with filter coefficients; **no `captureInternalState()` / `restoreInternalState()`** | **NO** — **BUG**: filter state (poles, delay lines) lost at page boundary |

### Key Observations

1. **Only `twTrackMix` and `twView` override `freezePage()`**. All others use the base class, which:
   - Installs a `FreezeContext` for cycle detection
   - Calls `renderFrames()` (default: calls `calcOutputTo()`)
   - Calls `captureInternalState()` / `restoreInternalState()` for state snapshots

2. **State continuity across page boundaries requires `captureInternalState()` / `restoreInternalState()`**. Components with internal playback state must implement these:
   - ✅ `twSampleReader` — play position
   - ✅ `twWavInput` — file offset
   - ❌ `twMoog` — **MISSING** — filter pole/delay state lost at page boundaries (audio quality gap, not causing silence)

3. **`twMixer` and `twRewire` are stateless**. The base class `freezePage()` via `calcOutputTo()` correctly handles them.

---

## Part 5: The Mutex Hold-Time Problem & Thread Safety

### The Silent Audio Issue

**Status:** Root cause identified but fix requires architectural changes.

**Problem:** Complete silence during playback despite correct signal wiring.

**Root Cause Chain:**
1. Read-ahead thread enters `freezePage_nolock(root)` → calls `calcOutputTo()` on twRewire/twMixer
2. `calcOutputTo()` acquires `mutex()` (line 52 in twrewire.cc, line 67 in twmixer.cc)
3. Calls `readStreamingData()` which recursively freezes all upstream components
4. Lock held for entire render: **10-50ms** for multi-track projects
5. Audio callback fires every ~21ms and tries `getPageIfExists()` with `std::try_to_lock` (line 295 in twcomponent.cc)
6. `try_to_lock` fails when mutex is held → returns `nullptr` → silence
7. With overlapping callbacks and page renders, most callbacks return silence

### Why A Naive Fix Introduces Use-After-Free

A tempting fix is to release the mutex before `readStreamingData()`:

```cpp
// UNSAFE — introduces use-after-free race condition
twLatchStreamingOutput* inputPlug = nullptr;
{
    std::lock_guard<std::mutex> lock(mutex());
    if (idx >= 0 && idx < nInputs_ && pInputPlugs[idx])
        inputPlug = static_cast<twLatchStreamingOutput*>(pInputPlugs[idx]);
}
// Lock RELEASED
inputPlug->readStreamingData(...);  // DANGEROUS: inputPlug may be dangling
```

**The Race:**
1. Read-ahead thread snapshots `inputPlug` under lock
2. Read-ahead thread releases lock
3. UI thread acquires lock, modifies signal chain via `setNInputs_nolock()` (line 138 in twmixer.cc)
   - Reallocates `pInputPlugs` array (line 153)
   - Frees old array (line 155)
4. Read-ahead thread dereferences now-dangling `inputPlug` → use-after-free crash or memory corruption

**Comments in code flag this risk:**
- twrewire.cc line 75-78: "CRITICAL: Must be called under lock because: 1. Reallocates pInputPlugs array (use-after-free race with calcOutputTo)"
- twmixer.cc line 136-137: "CRITICAL: Must be called under lock to prevent use-after-free when calcOutputTo() is running concurrently"

### Proper Long-Term Solution: Shared Pointer Hierarchy

To fix both the silence AND the thread safety race, the entire component hierarchy needs lifetime management:

1. **Use `shared_ptr<twLatchOutput>` for plugs array**
   - Snapshots hold a `shared_ptr` after release, keeping the plug alive even if array is reallocated
   - File: `tw303a/include/twcomponent.h` (pInputPlugs declaration)

2. **Use `shared_ptr<twComponent>` for all component references**
   - Ensures components stay alive during rendering even if signal chain is rewired
   - Especially critical for twView's wrapped component

3. **Scoped pointer snapshot pattern**
   - Brief lock to acquire shared_ptr copies
   - Release lock
   - Use shared_ptr freely without risk of dangling pointers

**Example (pseudo-code):**
```cpp
std::shared_ptr<twLatchStreamingOutput> snapshot;
{
    std::lock_guard<std::mutex> lock(mutex());
    if (idx >= 0 && idx < nInputs_ && pInputPlugs[idx])
        snapshot = pInputPlugs[idx];  // shared_ptr copy holds reference
}
if (snapshot)
    snapshot->readStreamingData(...);  // Safe: plug kept alive by snapshot
```

### Secondary Issue: twMoog Missing State Snapshot

Lower priority than the mutex problem, but worth noting: `twMoog` has no `captureInternalState()` / `restoreInternalState()`, so filter pole state is lost at page boundaries. This causes audio quality degradation but not silence.

---

## References

- **Architecture V3:** `docs/UNIFIED_RENDERING_ARCHITECTURE_V3.md`
- **Signal routing:** `tw303a/include/twcomponent.h`, `tw303a/include/twlatch.h`, `tw303a/include/twplug.h`
- **Freezing mechanism:** `tw303a/src/twcomponent.cc`, `tw303a/src/tw_freeze_context.cc`
- **Audio engine:** `tw303a/src/audio/audio_engine.cc`, `tw303a/src/twspeaker.cc`
- **Model wiring:** `main/src/sproject.cpp`, `main/src/sstdmixer.cpp`, `main/src/strack.cpp`
- **Known thread-safety issues:** Lines 75-78 in twrewire.cc, 136-137 in twmixer.cc
