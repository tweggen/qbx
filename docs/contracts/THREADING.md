# THREADING — thread inventory and the rules that keep it alive

## Thread inventory

| Thread | Created by | Runs | May touch Qt? |
|---|---|---|---|
| UI / main | Qt | all widgets, actions, model mutation, topology changes | yes |
| Audio callback | platform backend (WASAPI/ALSA/CoreAudio) | `twSpeaker` render callback → `AudioEngine::pullBlock` | **NO** |
| Readahead | `AudioEngine::startReadahead` | pre-buffers engine output | **NO** |
| Render worker | `RenderSession::start` | sequential freezePage + file writing | **NO** |
| Record worker | `RecordingSession::start` | device capture → resample → WAV writers | **NO** |
| Revalidator pool | `CaptureRevalidator` (N workers) | page recompute via `IRevalidatable` | **NO** |
| Buffering monitor | `twSpeaker::startOutput` | polls readahead, starts backend | **NO** |
| MIDI out scheduler | `MidiOutScheduler::start` | drains an SPSC ring, sends each message AT its due time (or hands it to a timestamping driver early) | **NO** |
| MIDI device thread | the platform MIDI input (WinMM callback / CoreMIDI read proc / ALSA-seq poll) | delivers received bytes to `MidiInputCallback` | **NO** |
| MIDI-out pump | `SApplication`'s `SMidiOutPump` QTimer (20 ms) | reads the playhead, slices each MIDI-out track's event FEED over a 250 ms window, enqueues `{dueHostTimeNs, bytes}` into `MidiOutScheduler` | yes (it IS the main thread) |

The MIDI-out pump is on this list to make the SEAM explicit: it is the single producer into `MidiOutScheduler`'s lock-free ring, and it is on the main thread, so everything above the ring may use Qt freely and everything below it may not. MIDI-out is emitted at PLAY time and only at play time — never at freeze time (proposal 37 D6, the proposal-34 metering lesson verbatim: pages are frozen ~1.4 s ahead of the playhead, and by renders that have no playhead at all).

## Rule 1 — no Qt off the main thread. Ever.

A raw `std::thread` that emits a Qt signal (or otherwise touches QObject
machinery) makes Qt ADOPT the thread; the adopted thread's Qt-TLS cleanup
then runs during DLL `THREAD_DETACH` at thread exit and deadlocks the
`join()` in teardown (empirically: `twSpeaker::stopOutput`). This is why:

- workers publish positions via `std::atomic` stores only
  (`SApplication::setGlobalLocatorPosRealtime`); a main-thread `QTimer`
  (`pumpLocator`) turns stores into repaints;
- session callbacks (`onPosition`, `onProgress`, `onComplete`) are invoked
  ON the worker thread — handlers must be lock-free/atomic and Qt-free;
  UI dialogs POLL the session's query methods from the GUI thread instead;
- `audio::PlaybackContext::locatorHeldElsewhere()`/`publishPosition()` are
  called on the audio callback thread — implementations are atomic ops.

The metering pump (proposal 34) is the SECOND instance of the same pattern
and needs no new machinery on the producing side at all: nothing off the main
thread publishes a level. `SApplication::pumpMeters` (`meterTimer_`, 33 ms,
self-stopping after a decay tail) reads the same atomic the playhead reads,
subtracts the device latency, and emits `meterTick`; each meter then does its
own page read and its own ballistics ON THE MAIN THREAD. Two consequences
worth stating:

- **A UI-thread `twComponent::getPageIfExists()` read is SANCTIONED.** It
  takes the component mutex with `std::try_to_lock` and returns `nullptr`
  rather than blocking, so it cannot stall the UI and cannot deadlock against
  a freeze in flight. A lost try-lock is indistinguishable from an absent
  page, which is why `twLevelProbe` keeps the previous tick's page.
- **Reading a page's samples while a worker re-freezes it in place is an
  ACCEPTED race**, not a bug to fix: `getOrAllocatePage` re-renders into the
  same buffer and `validFrames` is a plain `uint32_t`. `samples` is sized once
  in the constructor and never resized, so a clamped read stays in bounds and
  the worst case is one visually wrong meter frame. The audio thread already
  runs exactly this race (proposal 16's stale-page playback).

## Rule 2 — snapshot, don't lock, on the audio path

State handoff UI→audio is snapshot/double-buffer based:

- `SCut`: window params under the object mutex; audio takes
  `getSnapshot()` (try-lock with last-good fallback); the reader chain is
  double-buffered (`currentReader_` swapped atomically, shared_ptr
  refcounts keep the old chain alive for in-flight snapshots).
- `SObject` page cache: `currentPage_` read via `std::atomic_load`;
  the revalidator builds `nextPage_` privately and swaps under the object
  mutex (`revalSwapPages_nolock`).
- `twSpeaker`: the render callback captures `audioEngine_` as a local
  `shared_ptr` copy; the handle itself is guarded by `engineMutex_`.

## Rule 3 — lock discipline

- One mutex per object (`twComponent::mutex()`, `SObject::mutex()`);
  `_nolock` suffix = caller must already hold it. Never call a locking
  method from a `_nolock` one.
- `twSpeaker::engineMutex_` is a LEAF lock: held only to read/write the engine
  handle, never across blocking work — `~AudioEngine` joins the readahead
  thread and must run with no lock held (detach the handle, destroy the
  local copy outside).
- `freezePage` never holds the component mutex during rendering (upstream
  recursion would deadlock; the mutex is not recursive). Cache check and
  placeholder insertion only.
- Cross-object lock ordering is avoided rather than defined: snapshot the
  pointer/handle under the small lock, work on the copy with no lock held.

## Rule 4 — fixes must be order-independent

For races, remove the latch/assumption so the system self-heals under ANY
ordering — do not force a particular ordering (established project rule;
see plan/STATE.md sessions and `feedback` memory). Tests that only pass
for one interleaving are bugs.
