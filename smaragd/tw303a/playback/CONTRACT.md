# tw/playback — CONTRACT

Purpose: realtime playback. twSpeaker (device lifecycle state machine +
render callback), AudioEngine (graph pull, loop boundaries, device-rate
resampling), readahead buffering, and — since proposal 21 L1a — THE LIVE LANE:
the LiveGraphPump, the position-stamped ring the RT sums, the live clock and
the immutable live plan.

Public headers: twspeaker.h, audio_engine.h, playback_context.h (the
app-implemented services interface), twliveclock.h, twlivering.h, twliveplan.h,
twlivepump.h. audio_readahead.h retired at proposal 36
B5: AudioReadaheadBuffer was a std::deque<AudioFrame> with no callers anywhere
in the tree, and AudioFrame -- whose MAX_CHANNELS == 2 was the engine's hard
stereo cap -- went with it.

Depends on: tw/core, tw/pages, tw/graph, tw/devices, tw/sources (resampler),
tw/schedule, and — since proposal 21 L1a — tw/plugins and tw/mix. The pump
renders a live-owned track BLOCK-WISE, outside the frozen-page machinery, and
the three pieces of the graph that survive block-wise are exactly
twPluginSlotProcessor::render, twGainStage::applyGain and twRewire's channel
map. Neither module depends on playback, so the DAG stays acyclic.
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

11. THE TWO-LANE MACHINE (proposal 21 L1a, design D5). Three independent axes:
    device {CLOSED, OPEN} x frozen {IDLE, BUFFERING, PLAYING} x live {OFF, ON},
    and

        out = (frozen == PLAYING ? root : 0) + (live == ON ? ring : 0)

    - `openLive()` opens the device AND starts the backend immediately: there is
      no readahead behind a live lane and nothing to buffer, so the performer
      hears the input the moment they arm.
    - `startOutput()` ATTACHES when the device is already open — no second
      openDevice (which would clear a capture backend's recording), no second
      setRenderCallback. The callback is registered ONCE, by ensureDeviceOpen().
    - The readahead priming gates THE FROZEN LANE'S PLAYING and never the ring.
    - `stopOutput()` stops the LANE only while live is ON; the device stays open
      and the callback keeps summing the ring.
    - `closeLive()` closes the device iff the frozen lane is idle. The PUMP must
      already be stopped — the app owns it, and closing under a running producer
      would leave it writing into a ring nobody drains.
    - With live OFF every path is exactly what it was: Play opens, priming
      defers the backend start, Stop closes.

12. THE ENGINE HANDLE IS READ AND WRITTEN ATOMICALLY. A new AudioEngine can now
    be minted while the callback is RUNNING (Play attaching to a device the live
    lane opened), so `audioEngine_` is accessed through std::atomic_load /
    std::atomic_store everywhere. engineMutex_ stays a LEAF and is never taken
    by the callback. Before L1a a plain assignment was safe only because the
    device could not be running yet.

13. THE RING IS A STREAM OF POSITION-STAMPED FRAMES, NOT A QUEUE OF BLOCKS,
    and the RT sum is a PURE FUNCTION (`twlive::mixStream`, extracted for the
    same reason `twmonitor::pullChannels`/`interleave` are).

    THE STAMP IS A RANGE, NEVER AN EQUALITY. The RT's block is VARIABLE on a
    real device — WASAPI asks the callback for `bufferFrames - padding`, so its
    grid is irregular by construction (design F6) — while the pump produces
    fixed-size blocks. An RT that summed an entry only when
    `entry.startPos == theFrameBeingDelivered` would align with the pump
    exactly never on hardware: every entry a mismatch, the live lane
    permanently silent. So the consumer holds a CURSOR into the head entry
    (`twLiveMixReader`, consumer-private, and it must live across callbacks) and
    for a wanted range [P, P+n):
      - entries wholly BEHIND P are dropped and counted (`dropped`);
      - an entry starting AFTER the current want position is the FUTURE and is
        KEPT — silence for the gap, counted `notYet`. Popping it would throw
        away audio the very next callback needs;
      - an overlap is summed from the entry's own offset and the entry is
        popped only when exhausted. One 2048-frame callback consumes two
        1024-frame entries; a 33-frame one consumes a sliver.
    `frames` may differ from `framesPerEntry` in BOTH directions.

    THE RUN ID is what makes "keep the future" safe. A REPOSITION abandons a
    timeline, and everything queued for it is arbitrarily far from where the RT
    now is, so the keep-the-future rule would hold it forever: the ring fills,
    the producer can never write the new position, and the lane stops dead
    (measured, on a seek back and on the STOPPED->PLAYING switch). The producer
    stamps a monotone run id, bumps it in `applyReposition()` and publishes it
    with `setRun()`; the consumer DROPS any entry not of the current run.

    THE EPOCH GATE is evaluated PER ENTRY against the ROOT PAGE IN HAND, so a
    flip that lands mid-entry takes effect mid-entry:
      the served root page's contentEpoch >= the entry's `flipEpoch` (ARM: a
      stale root still CONTAINS the armed track, so summing would double it),
      or, on the DISARM mirror, still < `flipEpochPrime` (a stale root still
      LACKS the track, so the ring keeps filling the hole).
    While STOPPED there is no root page and the ring is the only position
    authority: out = ring, consumed sequentially from the head. A 2-3 ms
    crossfade smooths both flips and carries across entries and callbacks.
    `AudioEngine::servedContentEpoch()` publishes the epoch: it is the page the
    RT is already holding, never a second lookup. `twlive::mixRing` survives as
    the one-entry primitive with the strict position claim, used by tests.

14. THE LIVE CLOCK IS ENGINE-OWNED (twliveclock.h). A seqlock stamped by the
    render callback beside publishPosition() with
    `{seq, deliveredFrame = published - bufferFramesProject, nextFrame =
    published, hostNs}`. NOT PlaybackContext (app-implemented, UI-thread
    locatorPosition) and not SApplication (unreachable from tw/). The
    publish-lag correction is applied ONCE, here, so every consumer shares one
    definition — the same one SMidiOutPump derives for its own anchor. Readers
    retry a BOUNDED number of times and then report "no reading"; the pump is
    realtime too.

    TWO READINGS, DELIBERATELY NOT ONE. `deliveredFrame` is what is being
    HEARD and is what MIDI-out and metering want; `nextFrame` is what the RT
    will PULL next and is what the PUMP paces on. They differ by one device
    buffer, and conflating them would put the live lane a buffer behind the
    arrangement.

15. THE PUMP NEVER RENDERS A PAGE AND NEVER ALLOCATES IN STEADY STATE. It marks
    itself `markLiveThread()` (RenderPolicy::Never), takes only a live-owned
    processor's own mutex_ and `getPageIfExists`'s try-lock, and pushes the
    ring. All per-plan state — scratch (owned by the plan), retained pages,
    pointer arrays — is allocated at PLAN ADOPTION, at the top of a block. A
    full ring is a counted DROP; it is never grown and never waited on. The
    plan's transport is pushed onto every processor at adoption, so the plan and
    the processors cannot disagree for a block after a rebuild.

16. THE PUMP PACES ON THE CLOCK, and FILLING THE RING UNTIL IT IS FULL IS
    WRONG. While PLAYING it keeps `[nextFrame, nextFrame + leadFrames)` covered
    — rendering while `nextPos < nextFrame + lead` and idling otherwise. The
    original fill-until-full pump ran the ring's whole depth ahead, so the very
    next clock stamp read as a multi-block BACKWARDS jump and it repositioned,
    forgot continuity and re-rendered the covered range on every start. Pacing
    on the frames the RT actually wants removes the failure mode instead of
    widening a tolerance past it. `leadFrames < 0` means "default", which is
    TWO blocks; ZERO is a legal explicit value. `requiredRingDepth()` is
    `ceil(lead/block) + 2` and the pump warns once if the ring it was handed is
    shallower.

17. ONE EXPLICIT REPOSITION per start/stop/seek/wrap, decided by the pump and
    applied THROUGH THE PLAN (`applyReposition()`: move the position, count it,
    open a new RING RUN, `forgetContinuity()` on every plan processor), never by
    the app — which is not on this thread and does not know where a block
    boundary is. The rules are positional, not a drift tolerance:
      `nextPos < nextFrame`                    fell behind, or a seek FORWARD
                                               past the covered range;
      `nextPos > nextFrame + lead + block`      the clock moved BACK (a seek back
                                               or a loop wrap);
      `requestReposition()`                     the app said so — the transport
                                               knows before any clock reading
                                               can show it.
    A jump INSIDE the covered window needs none: the RT drops what it passed and
    streams on. The reposition is applied BEFORE the ring slot is claimed, so a
    full ring cannot lose it — which matters most in the case a reposition
    creates, where the ring is full of the run being abandoned.

18. THE LIVE LANE REQUIRES DEVICE RATE == PROJECT RATE, and `openLive()`
    REFUSES otherwise (-1, one log naming both rates, `liveRateRefusals()`, and
    the device closed again iff the frozen lane is not using it). The lane's
    entries are stamped in PROJECT frames and summed straight into the device
    buffer; the frozen lane has a resampler at this seam and the live lane has
    nowhere to put one, because a resampler makes an entry's frame count
    fractional while an entry has to carry a position. KNOWN DEBT — the
    resolution path is a device-frame-stamped ring, or opening the device at the
    project rate (ASIO, proposal 35).

How to test: manual GUI playback (scripted toggle-playback segfaults under
the runner — pre-existing, see the headless-testing notes); the render path
shares the graph but not this module.

Known debt: fixed buffer sizing (no user latency control); the
scripted-playback crash. (The callback's two per-block vector allocations are
gone since proposal 21 L1a — the ring sum needs the PLANAR buffers anyway, so
they became members sized at device open.) NOT gated: the real-device
behaviour of the two-lane machine (WASAPI), and any latency number for it.

**`twSpeaker`'s one input plug carries NO AUDIO — only the wire format.** It had
two until proposal 36 B5 and neither ever carried a sample: the callback reads
frozen pages through `AudioEngine`, which asks the ROOT COMPONENT directly, so
plug 0 exists to answer `getFormat().sampleRate` when the device is opened (and
for the rate diagnostic) and plug 1 was read by nobody at all. B9 reviewed it
for deletion as dead scaffolding and did NOT delete it: unlike `getDataPtr()`
and `twFormatCaps::channelCounts` it is READ, so removing it is "find the wire
rate somewhere else", not a cleanup. Recorded here so the next reader does not
spend the same half hour concluding it is dead.
