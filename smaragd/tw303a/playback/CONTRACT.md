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
5. Loop wrap [start, end) happens in the engine pull, atomics only. The wrap
   rewrites the playhead without walking the page cursor there, so:
6. updateFrozenPage() DERIVES the whole read cursor (currentPageStartPos_,
   pageFrameOffset_, cachedPageValidFrames_, currentPageGeneration_) from the
   page it ends up holding and the position it was asked for — on every exit,
   including the fast path. No caller may carry pageFrameOffset_ across a
   position jump; the batch loops' `+= batchSize` is an optimization that must
   agree with the derived value, never the source of truth. A cycle region
   inside one 65536-frame page is the case that proves it: the wrap target is
   the page already held, so a carried cursor replays from the pre-wrap offset
   while the playhead reports loopStart. Gate: the "cycle:" block in
   playback/tests/playback_test.cc.
7. A page found in the cache is trusted only if its OWN startPosition matches
   the position asked for — never the map key it was found under.

How to test: manual GUI playback (scripted toggle-playback segfaults under
the runner — pre-existing, see the headless-testing notes); the render path
shares the graph but not this module.

Known debt: fixed buffer sizing (no user latency control); callback
allocates two vectors per block; the scripted-playback crash.
