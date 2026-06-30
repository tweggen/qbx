# Plan 12: Decouple twTrackMix from the SObject Timeline

## Context

The user identified three structural problems in the rendering architecture:

**(a)** An old streaming pipeline (`calcOutputTo` chains down the twComponent tree)  
**(b)** A new page-backed pipeline (`CaptureRevalidator`, `CapturePageData`, `twOutputPage`)  
**(c)** The SObject child list used as the live DSP timeline — `twTrackMix` reaches into the SObject world on every audio buffer

The thesis: solving **(c)** removes the need for the special-casing that makes **(a)** and **(b)** coexist messily, and paves the way toward a single, clean, twComponent-only page-based rendering pipeline with no rendering responsibilities in the SObject hierarchy.

---

## Confirmed Problems (from code analysis)

### Three rendering systems coexist

| System | Used for | Files |
|--------|----------|-------|
| `twComponent::freezePage()` / `twOutputPage` | Real-time audio (AudioEngine → twRewire root only) | twcomponent.cc, audio_engine.cc |
| `SObject::currentPage_` / `CapturePageData` / `CaptureRevalidator` | Preview waveforms only; `recomputePlayback()` declared but never overridden anywhere → dead | sobject.h, capture_revalidator.cc |
| `SCut::capture_` / `twCapturingSource` / `renderObjectInto()` | Container-backed and grained cuts; what the live audio path actually uses | scut.cpp:293–373 |

`recomputePlayback()` is called by the revalidator but never overridden — any Playback job fills nothing in the page and the page stays invalid, causing an infinite reschedule loop.

SCut has **shadowed page fields** (`currentPage_` / `nextPage_` declared at scut.h:350 and again at sobject.h:381). The revalidator swaps the base-class fields; `SCut::getPreviewCapture()` reads SCut's own shadow. They are different objects.

### The structural coupling: twTrackMix → SObject

`twTrackMix` holds `STrack &track_` (twtrackmix.h:49). On every audio buffer it calls:
- `track_.childLinks()` — reads SObject's child list (twtrackmix.cc:121)
- `lk->getStartTime()` — reads SLink timeline position (twtrackmix.cc:123)
- `lk->getRootComponent().calcOutputTo(...)` — calls through SLink into SObject (twtrackmix.cc:152)

And on every seek:
- `track_.childLinks()` again (twtrackmix.cc:38)
- `lk->getStartTime()` again (twtrackmix.cc:40)
- `lk->seekTo(clipRelative)` (twtrackmix.cc:46)

This cross-boundary reach is what prevents the `tw303a/` engine library from being self-contained. `twtrackmix.cc` currently `#include`s `sobject.h`, `slink.h`, `strack.h`, and `sstdmixer.h` — all from `main/`.

### Downstream consequences of the coupling

- `renderObjectInto()` (scut.cpp:219) exists because the twComponent tree doesn't own clip layout — it replicates layout offline in an ad-hoc recursive renderer used by `buildCapture_()`.
- `buildCapture_()` (scut.cpp:293) can be called on the audio thread (first pull via `ensureReader()` → `getRootComponent()`), causing a potentially long synchronous render on the hot audio callback path.

---

## Target Architecture

```
SObject/SXxx hierarchy          twComponent hierarchy
(UI / model only)               (DSP only, self-contained)
─────────────────               ──────────────────────────
SStdMixer ──────────────────→  twRewire
  STrack ──── notifies ──────→  twTrackMix  ← owns ClipEntry list
    SCut ────── notifies             (startTime, duration, twComponent*)
    SPlainWave                   twSampleReader / twLoopReader / twGrainSource
                                 twWavInput / twResampledSource
```

Key invariants:
1. **twTrackMix owns its own timeline** — a sorted list of `(startTime, duration, twComponent*)` entries maintained by STrack push notifications.
2. **No twComponent reads SObject or SLink fields at render time.**
3. **tw303a/ has zero includes of main/ headers** (currently violated by twtrackmix.cc).

---

## Phase 1 (This Plan): Give twTrackMix Its Own Timeline

This is the minimum change that breaks the structural coupling without touching any rendering algorithms.

### Changes to `smaragd/tw303a/include/twtrackmix.h`

Add a `ClipEntry` struct and clip-management methods. Remove `STrack &track_`:

```cpp
struct ClipEntry {
    offset_t     startTime;
    length_t     duration;    // 0 = unbounded
    twComponent *component;   // borrowed; lifetime managed by STrack
};

// Called by STrack on the UI thread when clips are added/moved/removed.
void insertClip(offset_t startTime, length_t duration, twComponent *comp);
void removeClip(twComponent *comp);
void updateClip(twComponent *comp, offset_t newStartTime, length_t newDuration);

private:
    std::vector<ClipEntry> clips_;   // replaces STrack &track_
    // mutex() from twComponent base protects clips_ (same lock already used)
```

The constructor signature changes from `twTrackMix(env, STrack&)` to `twTrackMix(env)`.

### Changes to `smaragd/tw303a/src/twtrackmix.cc`

- Remove `#include` of `sobject.h`, `slink.h`, `strack.h`, `sstdmixer.h`.
- `calcOutputTo_nolock`: iterate `clips_` instead of `track_.childLinks()`. Logic is identical — only the data source changes.
- `seekTo_nolock`: same substitution.
- Add `insertClip`, `removeClip`, `updateClip` implementations (simple vector operations under `mutex()`).

### Changes to `smaragd/main/src/strack.cpp`

**`setNBusses`**: constructor call changes from `new twTrackMix(env, *this)` to `new twTrackMix(env)`. After creating each mixer, populate it with the current child list (initial sync, UI-thread-only, runs before audio starts):

```cpp
for( SLink *lk : childLinks() ) {
    if( !lk->hasStartTime() ) continue;
    newMixers[i]->insertClip(
        lk->getStartTime(),
        lk->getSObject().hasDuration() ? lk->getSObject().getDuration() : 0,
        &lk->getRootComponent() );
}
```

**`trackChildWasAdded`** (strack.cpp:159): after existing signal wiring, call `insertClip` on all bus mixers:

```cpp
for( int i=0; i<nBusses_; i++ )
    cpTrackMixers_[i]->insertClip(
        child.getStartTime(),
        child.getSObject().hasDuration() ? child.getSObject().getDuration() : 0,
        &child.getRootComponent() );
```

**`trackChildWasRemoved`** (strack.cpp:180): call `removeClip` on all bus mixers, passing `&child.getRootComponent()` as the key.

**`trackChildWasMoved`** (strack.cpp:145): call `updateClip` on all bus mixers with the new start time and duration.

Duration changes on children also need to forward to `updateClip`. The existing `trackChildDurationChanged` slot (strack.cpp:138) already knows the child via the Qt sender; extend it to call `updateClip` on all bus mixers with the updated duration.

### What stays exactly the same

- `twTrackMix::calcOutputTo_nolock` logic (only data source changes from `track_.childLinks()` to `clips_`)
- SCut, rebuildReader, buildCapture_, capture_, renderObjectInto — untouched
- AudioEngine / freezePage / twSpeaker path — untouched
- All signal/slot wiring in STrack for duration and start-time tracking

---

## Phase 2 (Future): Cleanup enabled by Phase 1

Once twTrackMix is decoupled:

- **Remove `tw303a/` dependency on `main/`** entirely (enforce via CMake `target_include_directories`).
- **Fix SCut shadow page fields** — remove duplicate `currentPage_`/`nextPage_` from scut.h; let SCut use the SObject base-class fields.
- **Remove dead `recomputePlayback` paths** — delete the `Playback` aspect from `CaptureRevalidator::dispatchRecomputation` and from all `invalidate*` calls. Preview-only revalidation is what actually runs.
- **Move buildCapture_() off the audio thread** — STrack can eagerly build the `twCapturingSource` on the UI thread when a container clip is added, and register it as the `ClipEntry::component` directly; `ensureReader()` no longer risks blocking the audio callback.

## Phase 3 (Future): Unified page-based rendering

- Unify `twOutputPage` and `CapturePageData` into one system.
- Extend `freezePage` cascading below the twRewire root so the entire chain is page-based.
- Remove `renderObjectInto()` entirely.

---

## Sequencing Constraints

1. **Phase 1 is prerequisite to everything else** — nothing else above is safe until twTrackMix doesn't read SObject at render time.
2. **Do not unify the two page systems until Phase 1 is stable and audio path confirmed clean.**
3. **SCut shadow field fix** can land as a cleanup commit after Phase 1 is merged.

---

## Verification

1. `./build.sh` — no new warnings; after Phase 1, grep confirms `tw303a/` contains no `#include "sobject.h"` or `#include "slink.h"` or `#include "strack.h"`.
2. Load a project with multiple tracks and mixed clip types; play from beginning and from a mid-project seek point.
3. While playing: add a clip, move it, delete it — verify clip layout updates correctly with no dropout or glitch.
4. Play a grained 44.1 kHz clip in a 48 kHz project — existing SCut/capture machinery is unchanged so offset fixes remain intact.
5. Run `./build/bin/action_roundtrip_test`.
