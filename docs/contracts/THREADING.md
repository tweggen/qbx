# THREADING — thread inventory and the rules that keep it alive

## Thread inventory

| Thread | Created by | Runs | May touch Qt? |
|---|---|---|---|
| UI / main | Qt | all widgets, actions, model mutation, topology changes | yes |
| Audio callback | platform backend (WASAPI/ALSA/CoreAudio) | `twSpeaker` render callback → `AudioEngine::pullBlock` | **NO** |
| Readahead | `AudioEngine::startReadahead` | pre-buffers engine output | **NO** |
| Render worker | `RenderSession::start` | sequential freezePage + file writing | **NO** |
| **Capture bridge** | `CaptureBridge::start` (proposal 21 L3a) | the input ring's ONE consumer: pop → resample → fan out to the live-lane ring, the `twGrowingCaptureSource` pages and (never a file) | **NO** |
| **Capture WAV writer** | `CaptureBridge::start` | reads the capture pages BY POSITION and writes every WAV sink. May be LATE (`wavLate`); at stop it finalises the files out of the pages. Deliberately NOT the bridge thread — a slow file must never stall the ring | **NO** |
| Record session (no app consumer since L3b) | `RecordingSession::start` | waits for the transport to stop, then calls `CaptureBridge::stop()`; wakes at 100 ms only to emit progress (the old 1 ms device poll is gone) | **NO** |
| **Input capture (one per open device)** | `AudioInput::startCapture` (WASAPI / ALSA / FileAudioInput; on CoreAudio the AVAudioEngine TAP plays this role and is not ours to create) | waits on the device's own event, pushes WHOLE packets into that device's SPSC ring (`tw/devices/audio_ring.h`) | **NO** |
| Revalidator pool | `CaptureRevalidator` (N workers) | page recompute via `IRevalidatable` | **NO** |
| Buffering monitor | `twSpeaker::startOutput` | polls readahead, starts backend | **NO** |
| MIDI out scheduler | `MidiOutScheduler::start` | drains an SPSC ring, sends each message AT its due time (or hands it to a timestamping driver early) | **NO** |
| MIDI device thread | the platform MIDI input (WinMM callback / CoreMIDI read proc / ALSA-seq poll), and `CaptureMidiInput::inject` / `KeyboardMidiInput::noteOn` on their CALLER's thread | delivers received bytes to `MidiInputCallback`. Since proposal 21 L2 that callback is `MidiInFanout::onMessage`, which fans out to ONE SPSC RING PER CONSUMER (design D8) and pushes MIDI-THRU straight into `MidiOutScheduler`'s IMMEDIATE ring | **NO** |
| MIDI-out pump | `SApplication`'s `SMidiOutPump` QTimer (20 ms) | reads the playhead, slices each MIDI-out track's event FEED over a 250 ms window, enqueues `{dueHostTimeNs, bytes}` into `MidiOutScheduler` | yes (it IS the main thread) |
| **Audio recorder** | `SApplication`'s `SAudioRecorder` QTimer (100 ms, proposal 21 L3b) | takes the placement anchor from `twEngineClock`, publishes the growing clip's length, checks the punch-out; at stop it ends the capture segment and submits ONE undo macro of `place-recording`. The BRIDGE THREAD never touches the model — it appends to the pages and stores a frontier, and this polls | yes (it IS the main thread) |
| **Live graph pump** | `LiveGraphPump::start` (proposal 21 L1a) | renders every live-owned track BLOCK-WISE — input ring / events → `twPluginSlotProcessor::render(positional)` per slot → `twGainStage::applyGain` → channel map → the folder sums → ONE position-stamped entry into `twLiveMixRing` | **NO** |

The INPUT CAPTURE thread is on this list for the same reason the MIDI-out pump is: it makes a SEAM explicit. It is the ONE producer into its device's ring and it does nothing else — no Qt, no allocation in the steady state, no lock, and never a call back into the app. Everything above the ring (`AudioInput::read`, which is a pop) may be an ordinary worker; everything below it runs at device priority. Before proposal 21 L0 there was no such thread: `read()` WAS the device poll, which is why a packet bigger than the caller's buffer lost its tail (design §1 F7) and why a consumer that was late lost audio rather than latency.

Its counterpart on the policy side is `twRtThreadGuard`, which since L0 carries a per-thread `RenderPolicy {Any, Never}` with TWO markers behind ONE check in `twComponent::freezePage`: `markRtThread()` (the audio callback — one-shot report plus a debug assert, a bug to fix) and `markLiveThread()` (proposal 21 L1a's pump — silence, a process-wide `liveThreadRefusals` counter and exactly one log line, never an assert, because a preview or an asset capture arriving at a live-owned component is recoverable by design).

The LIVE GRAPH PUMP is the newest thread on the list and the one with the tightest contract, because it sits between the two worlds rather than in either. It MAY take a live-owned processor's own `mutex_` (bounded — nobody else may render that processor while it is live-owned, which is what `setLiveOwned` enforces), `getPageIfExists`'s TRY-lock (a miss is silence for that input this block, counted, never a wait), push the live ring, and read the engine position atomic. It MUST NOT `freezePage`/`requestPage`/`fetchInputPage`/`copyData`, `requestGraphPages`, take any blocking component `mutex()`, allocate in the steady state, or touch Qt. It is `markLiveThread()` and MMCSS "Pro Audio". Everything per-plan it needs — scratch, retained pages, pointer arrays — is allocated when it ADOPTS a plan, at the top of a block, which is also where the previous plan is released ("the old plan is released after the pump's next block").

While PLAYING the pump paces on the CLOCK: it keeps `[nextFrame, nextFrame + lead)` covered and idles otherwise, where `nextFrame` is the frame the RT will pull next. Filling the ring until it is full is wrong and was the original defect — the pump then ran its whole depth ahead, and the next clock stamp read as a multi-block backwards jump that repositioned it on every start. While STOPPED, where there is no clock, the RT's DRAIN is the pacer. A full ring is a counted drop; a pump that waited would be a second thing the callback can block on.

`LiveGraphPump::renderOneBlock()` is public and synchronous precisely so the block-wise render can be compared to a frozen render without a device, a pacer or a ring consumer; it marks its CALLER as the live thread, and that marker is sticky, so a test that uses it must do so on a thread of its own.

On the CONSUMER side the contract is symmetrical and just as tight: the RT reads the ring as a STREAM by frame range, never by block equality, because its own block size is variable on a real device while the pump's is fixed. The cursor into the head entry is consumer-private state that must live across callbacks (`twSpeaker` holds it as a member), and an entry from a run the pump has abandoned is dropped on sight — without that, the consumer's keep-the-future rule would hold a stale queue forever and starve the producer.

`MidiOutScheduler` has TWO producer seams since proposal 21 L2 and they must not be
confused. `enqueue()` is SINGLE-PRODUCER and that producer is the MIDI-out pump on the main
thread; `sendImmediate()` is a SECOND, dedicated SPSC ring whose producer is a MIDI INPUT DEVICE
THREAD (MIDI-thru), drained FIRST in the sender loop, waking it at once, sent with due time 0 and
deliberately OUTSIDE `flush()`'s discard — a flush drops a queued FUTURE, and a thru byte is a key
being pressed right now. ONE producer per ring is the caller's guarantee: `MidiInFanout::setThru`
refuses a second target for a port, and the app routes at most one input port to a given scheduler.

`twLiveEventSource::collect()` runs ON THE LIVE GRAPH PUMP, inside
`twPluginSlotProcessor::render` — it is the only consumer of its ring, it allocates nothing in the
steady state (every vector is sized on the main thread), and it touches no Qt. The host-time →
frame mapping reaches it as one virtual call (`twLiveFrameClock`) so `tw/devices` need not include
`tw/playback`.

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

## Rule 5 — the run barrier is MAIN THREAD ONLY

`SApplication::beginRun(pos)` (proposal 37 P3c, design D4 / 4.4) walks the
model tree, clears every instrument processor's continuity and invalidates the
render path from `pos` to infinity. It reads the model, which belongs to the
main thread, and it must be ordered BEFORE the run's first demand — so it is
called from `startRender()` before the render session's thread exists, and from
every play-start path immediately before `twSpeaker::startOutput()`, which
performs the engine's pre-readahead `seekTo` + `startReadahead()` on that same
thread. There are three such play-start paths today (`SMainWindow::startPlaying`,
`SApplication::setPlaybackRunning`, and the monitoring playback inside
`SApplication::startRecording`) and each carries its own call: a barrier on one
of them only would make determinism depend on which button was pressed.

NEVER from the readahead thread, the RT callback or a scheduler worker. It is
not issued on a seek during playback or on a loop wrap either — not for
threading reasons but because the RT thread adopts a fresh page mid-page
(proposal 16), so re-staling what is being served would be an audible switch.
It is order-independent in rule 4's sense: a late barrier costs one re-render,
never a wrong page served as current.
