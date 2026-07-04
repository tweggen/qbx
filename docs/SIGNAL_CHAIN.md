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
2. **SStdMixer::setNBusses(n)** — creates per-bus infrastructure
   - Creates `twMixer[0..n-1]` (one per audio bus: e.g., stereo = 2)
   - Creates `twRewire` (output multiplexer)
   - Wires: `twMixer[i]->linkOutput(0)` → `twRewire->setInput(i, ...)`
3. **SStdMixer::reconnectTracksToMixer()** — for each track
   - Gets track's root component: `STrack::getRootComponent()` → twRewire
   - Wires: `twRewire[track]->linkOutput(bus)` → `twMixer[bus]->setInput(track, ...)`
4. **STrack::setNBusses(n)** — per-track infrastructure
   - Creates `twTrackMix[0..n-1]` (clips mixer per bus)
   - Creates `twPluginChain[0..n-1]` (effect inserts per bus)
   - Creates `twRewire` (output multiplexer)
   - Wires: `twTrackMix[i]` → `twPluginChain[i]` → `twRewire->setInput(i, ...)`
5. **STrack::trackChildWasAdded(SLink)** — when clip added
   - Calls `twTrackMix[bus]->insertClip(startTime, duration, getComponentFn)`
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
| 3a | SStdMixer twMixer[] created | main/src/sstdmixer.cpp | 219–286 | `setNBusses()` → `new twMixer` × N |
| 3b | SStdMixer twRewire created | main/src/sstdmixer.cpp | 276 | `new twRewire(env)` |
| 3c | Mixer→Rewire wiring | main/src/sstdmixer.cpp | 283–285 | `cpRewire_->setInput(i, mix->linkOutput(0))` |
| 4 | Tracks → Mixer wiring | main/src/sstdmixer.cpp | 134–169 | `reconnectTracksToMixer()` → `mix->setInput(bus, track_rewire.linkOutput(bus))` |
| 5a | STrack twTrackMix[] created | main/src/strack.cpp | 244 | `setNBusses()` → `new twTrackMix(env)` |
| 5b | STrack twPluginChain[] created | main/src/strack.cpp | 262 | `new twPluginChain(env, 1)` |
| 5c | STrack twRewire created | main/src/strack.cpp | 276 | `new twRewire(env)` |
| 5d | TrackMix→Chain→Rewire wiring | main/src/strack.cpp | 282–288 | `cpPluginChains_[i]->setInput(0, cpTrackMixers_[i]->linkOutput(0))` etc. |
| 6 | Clips inserted into twTrackMix | main/src/strack.cpp | 175–200 | `trackChildWasAdded()` → `cpTrackMixers_[i]->insertClip(startTime, duration, getComponentFn)` |
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
| **twConstant** | twconstant.cc | No (base class) | `renderFrames()` override (Tier 2 push) — returns constant DC value | None (stateless) |
| **twWhiteNoise** | twwhitenoise.cc | No (base class) | `calcOutputTo()` generates random samples | None (stateless for audio; PRNG state not captured) |
| **twOsc** | twosc.cc | No (base class) | Base class for oscillators; derived by `twWhiteNoise`, `twConstant` | None in base |
| **twTestSeq** | twtestseq.cc | No (base class) | `calcOutputTo()` generates note sequences | **Possibly** — sequence position (check code) |
| **twPluginInsert** | twplugininsert.cc | No (base class) | Wraps single plugin; chains via latch | Delegates to plugin |
| **twPipe** | twpipe.cc | No (base class) | Simple 1-in/1-out pass-through | None |

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

## Part 5: The Mutex Hold-Time Problem

### The Issue

`twRewire::calcOutputTo()` (line 50–66) and `twMixer::calcOutputTo()` (line 65–94) both acquire `mutex()` and hold it for the entire recursive upstream render:

```cpp
// twRewire::calcOutputTo at line 52
std::lock_guard<std::mutex> lock(mutex());  // ← acquired
...
readStreamingData(...)  // ← triggers FULL upstream graph render inside lock
```

When the read-ahead thread renders a page:
1. Enters `freezePage_nolock(root_rewire)` → calls `calcOutputTo()`
2. `twRewire::calcOutputTo()` acquires `root_rewire->mutex()`
3. Calls `readStreamingData()` which recursively freezes all upstream components
4. Hold time: 10-50ms for multi-track projects
5. Lock NOT released until entire graph render completes

Meanwhile, audio callback fires every ~21ms:
1. Calls `updateFrozenPage()` → `getPageIfExists()` with `std::try_to_lock`
2. `try_to_lock` fails (mutex held by read-ahead)
3. Returns `nullptr`
4. Audio thread outputs silence for that callback period

**Result:** Overlapping 21ms callbacks + 10-50ms lock holds = frequent silence gaps.

### The Fix

Release the mutex before calling `readStreamingData()`:

```cpp
// twRewire::calcOutputTo() — FIXED
length_t twRewire::calcOutputTo(IOVector& dest, idx_t idx) {
    twLatchStreamingOutput* inputPlug = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex());
        if (idx >= 0 && idx < nInputs_ && pInputPlugs[idx])
            inputPlug = static_cast<twLatchStreamingOutput*>(pInputPlugs[idx]);
    }  // ← lock released BEFORE readStreamingData
    if (!inputPlug)
        return dest.fillSilence(0, dest.length());

    std::vector<sample_t> buffer(dest.length());
    length_t readFrames = inputPlug->readStreamingData(buffer.data(), dest.length());
    return dest.copyFrom(IOVector::CreateFromBuffer(buffer.data(), readFrames), 0, readFrames);
}
```

Lock hold time reduced to microseconds (just reading pointer values).

---

## References

- **Architecture V3:** `docs/UNIFIED_RENDERING_ARCHITECTURE_V3.md`
- **Signal routing:** `tw303a/include/twcomponent.h`, `tw303a/include/twlatch.h`, `tw303a/include/twplug.h`
- **Freezing mechanism:** `tw303a/src/twcomponent.cc`, `tw303a/src/tw_freeze_context.cc`
- **Audio engine:** `tw303a/src/audio/audio_engine.cc`, `tw303a/src/twspeaker.cc`
- **Model wiring:** `main/src/sproject.cpp`, `main/src/sstdmixer.cpp`, `main/src/strack.cpp`
