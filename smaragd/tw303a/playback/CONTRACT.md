# tw/playback — CONTRACT

Purpose: realtime playback. twSpeaker (device lifecycle state machine +
render callback), AudioEngine (graph pull, loop boundaries, device-rate
resampling), readahead buffering.

Public headers: twspeaker.h, audio_engine.h, audio_readahead.h,
playback_context.h (the app-implemented services interface).

Depends on: tw/core, tw/pages, tw/graph, tw/devices, tw/sources (resampler).
Forbidden: tw/sinks (nothing here writes files), app headers — all app
knowledge flows through audio::PlaybackContext.

Invariants:
1. PlaybackContext is injected once at startup and outlives the speaker;
   rootComponent()/locatorPosition() are UI-thread, locatorHeldElsewhere()/
   publishPosition() are AUDIO-thread (atomic ops only, no Qt).
2. The speaker never publishes the locator while recording owns it
   (locatorHeldElsewhere).
3. OutputState machine: STOPPED→OPENING→BUFFERING→PLAYING→STOPPING; backend
   output starts only after the readahead reports ready (monitor thread).
4. engineMutex_ is a LEAF lock (THREADING.md rule 3): the engine handle is
   snapshotted as a shared_ptr copy; ~AudioEngine (joins readahead) runs
   with no lock held.
5. Loop wrap [start, end) happens in the engine pull, atomics only.

How to test: manual GUI playback (scripted toggle-playback segfaults under
the runner — pre-existing, see the headless-testing notes); the render path
shares the graph but not this module.

Known debt: fixed buffer sizing (no user latency control); callback
allocates two vectors per block; the scripted-playback crash.
