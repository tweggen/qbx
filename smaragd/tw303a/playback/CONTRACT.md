# tw/playback — CONTRACT

Purpose: realtime playback. twSpeaker (device lifecycle state machine +
render callback), AudioEngine (graph pull, loop boundaries, device-rate
resampling), readahead buffering.

Public headers: twspeaker.h, audio_engine.h, playback_context.h (the
app-implemented services interface). audio_readahead.h retired at proposal 36
B5: AudioReadaheadBuffer was a std::deque<AudioFrame> with no callers anywhere
in the tree, and AudioFrame -- whose MAX_CHANNELS == 2 was the engine's hard
stereo cap -- went with it.

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
8. pullBlock() serves N PLANAR BUFFERS (proposal 36 B5), and destination
   channel c is page channel `twPageClampChannel(page, c)` — the §4.4 read
   clamp, so a narrower page plays on every destination channel ("mono plays on
   every channel") and a stale narrow page served by proposal 16's fallback can
   never be an out-of-bounds read. Every exit path, including every miss path,
   writes all nFrames of every buffer. It used to be (outL, outR) with both
   filled from channelPtr(0).
9. MONITORING IS STEREO. twSpeaker owns the CHANNEL mismatch at the device
   boundary — the same seam it already owns the sample-rate mismatch at — and
   the rule is one line (requester decision, proposal 36 B5):

       L = ch0;   R = (projectWidth >= 2) ? ch1 : ch0

   and that PAIR then meets the device's own channel count exactly as it always
   has, alternating across however many outputs the device has. So a mono
   project is standard mono-to-stereo; a stereo project is itself; and a
   project WIDER THAN TWO is monitored on its FIRST TWO CHANNELS — the rest are
   computed in full and DROPPED AT THE DEVICE, deliberately, because a
   fold-down needs channel ROLES and a fold law that proposal 36 §8 names as an
   explicit non-goal. It is a decision a user can be told, not an
   implementation detail, and the callback LOGS IT ONCE per width (an atomic
   exchange, never a per-callback line) so somebody hearing four of their six
   channels missing can find out why without reading source.

   THIS IS THE DEVICE PATH ONLY. A render is RenderSession, in tw/render, and
   shares no code with this: it writes the project's FULL width (proposal 36
   AC B5.3 — a 6-channel project renders a 6-channel file). A 6-channel project
   monitored in stereo still renders six channels.

   The rule lives as a pure free function (`twmonitor::pullChannels` /
   `twmonitor::interleave`, twspeaker.h) precisely so it can be asserted
   without opening a device; playback_test does that at widths 1, 2 and 6
   against 2- and 6-channel devices. qxa.mc_playback_channels measures the
   whole path — capture backend against offline render, same numbers, same
   positions.
10. ONE twResampler PER CHANNEL. The class is documented as a converter "for a
   single mono channel" and carries interpolation phase plus an input history
   buffer; sharing one across channels would smear them together. The vector is
   sized in configureResampling(), never on the RT path.

How to test: manual GUI playback (scripted toggle-playback segfaults under
the runner — pre-existing, see the headless-testing notes); the render path
shares the graph but not this module.

Known debt: fixed buffer sizing (no user latency control); callback
allocates two vectors per block; the scripted-playback crash.
