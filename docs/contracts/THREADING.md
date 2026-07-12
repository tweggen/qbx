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
