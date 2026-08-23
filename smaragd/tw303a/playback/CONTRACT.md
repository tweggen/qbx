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

19. THE METRONOME IS A LIVE-LANE SOURCE, GENERATED BY POSITION (proposal 21
    L5, `twmetronome.h`). `twMetronomeSource` is a `twLiveInputSource` and
    nothing else: it is never a component, never in the frozen graph, and
    therefore NEVER IN A RENDER — `SLiveMonitor::suspendForRender()` drops every
    lane, so a project rendered with the click on is byte-identical to the same
    project rendered with it off (`metronome_render_identity.qxa`).

    It is a PURE FUNCTION OF THE BLOCK POSITION out of an immutable tempo /
    time-signature snapshot. There is no "next click" cursor to get out of step
    with a seek, a loop wrap, a reposition or a count-in, and a click straddling
    a block boundary is completed by the next block because both blocks compute
    the same answer for it. A tempo or level edit builds a NEW source and
    republishes the plan — the same discipline the plan's gain envelope and
    channel map follow — and `pull()` is allocation-free (the two click
    waveforms are rendered once, in the constructor) and takes no lock.

    The beat length is a REDUCED RATIONAL and `frameOfBeat(k)` is ONE floored
    division, so beat k is never the accumulation of k roundings. The beat is
    one note of the time signature's DENOMINATOR (a quarter in 4/4, an eighth in
    6/8) and the accent is beat 1 of a bar, which is what every DAW's click
    does.

    THE ACTIVE RANGE `[rangeStart, rangeEnd)` is what makes "exactly N bars of
    click" a property of the waveform rather than a race: the pump renders one
    to two blocks AHEAD of the RT, so a count-in ended by a "stop clicking now"
    call would always let the downbeat past its end through, and the count would
    be N*beats or N*beats+1 depending on the box.

20. `twLiveMixRing::framesDelivered()` COUNTS FRAMES THE RT ACTUALLY SUMMED,
    cumulatively (proposal 21 L5). It is the only wall-clock-free measure of how
    far a STOPPED live lane has got — while stopped there is no engine clock and
    no root page, so the ring IS the transport — and it is what a count-in ends
    on. A QTimer would measure the Windows scheduler (15.6 ms of granularity)
    against a beat grid the same gate asserts to within 38 frames.

    MEASURED CONSEQUENCE OF INVARIANT 17, first seen from the outside here
    (proposal 21 L5). A live lane coming up on a STOPPED->PLAYING transition
    costs exactly one reposition: the pump starts at the locator while the
    engine clock is still invalid and delivers audio for it, the frozen lane
    then primes and publishes, and the pump repositions onto the publication -
    abandoning a run whose queued entries the consumer drops. At
    `SMARAGD_REVAL_WORKERS=1` on the reference box the audio at frame 0 came out
    EARLY in about **1 run in 50** and was swallowed in the other 49, with a
    **~5087-frame (106 ms) hole** after it either way; the steady stream after
    that is exact to five frames. A monitored INPUT pays the same cost and
    simply has no onset to make it audible, which is why it took a metronome to
    see it. It is the model's price, not a defect — recorded so the next reader
    does not diagnose it twice.

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

### The frozen lane can be WARMED before the transport moves

`startOutput()` mints a NEW `AudioEngine` whose readahead frontier starts at
0, and `AudioEngine::startPlayback()` will not let the frozen lane play until
that frontier covers `AudioEngine::primingFrames()` beyond the playhead. On a
heavy project that is SECONDS of running transport with nothing audible
(~2.3 s measured on a real one).

`twSpeaker::warmFrozenLane(pos)` demands exactly that window at `pos` through
the page scheduler, from the MAIN THREAD, before the transport moves. It works
for one reason and a change that breaks it breaks this: **the readahead
PROBES**. `readaheadLoop` asks `getPageIfExists` and, for a page that exists
and is current, advances its frontier and moves on WITHOUT re-freezing — so a
pre-warmed window turns the wait into a walk. A readahead that unconditionally
re-froze would make the warm-up worthless.

Three properties it must keep:

* **It never renders and never blocks.** It issues a demand and keeps the
  handle only to report on; dropping the handle does not abort the work.
* **Priority 9, the readahead's own.** This IS the readahead's work, done
  early; at a lower priority the background aspect jobs would spend the
  lead time on something nobody is waiting for.
* **The window is derived from `primingFrames()`**, never from a second
  constant. One authority for the number, so a change to the wait cannot
  leave the warm-up covering less than the wait requires.

The caller decides WHEN there is lead time to spend. Today exactly one does:
a record COUNT-IN (`main/shell/CONTRACT.md` inv. 37). A plain Play from
stopped has none and still pays the priming in full.

**The readahead's first pass is immediate.** Its 20 ms wait is skipped on the
first iteration only — the frontier is reset to 0 by `startReadahead()`, so an
unconditional wait there made a fully-warm start still report BUFFERING on the
first poll. Every `continue` in the loop still lands on a wait, so this cannot
become a spin.

### The readahead demands ACROSS a cycle wrap (fix/loop-behaviour, issue h, 2026-08-23)

**Read this before touching the readahead loop or `startPlayback()`'s
buffering check — both were wrong in ways a single-page loop cannot show.**

The readahead window (`readaheadLoop()`) used to be purely linear in the
playhead: `pos = pageStart + i*pageSize` for `i` in `[0, pagesNeeded)`, with no
reference to `loopEnd_`/`loopStart_` at all. Approaching a cycle's end, the
whole ~3 s window was spent on pages PAST `loopEnd_` that could never play, and
no page at `loopStart_` was ever demanded ahead of the wrap. A loop that fits
inside one 65536-frame page never shows this — inv. 5/6 above already cover
that shape — because `updateFrozenPage(loopStart)` finds the SAME page it
already holds regardless of what the readahead's window logic does. A loop
spanning more than one page is what breaks: `playback_test.cc`'s "Cycle wrap
ACROSS MULTIPLE PAGES" case is the first regression test with that shape, and
`qxa.playback_loop_wrap_continuity` gates the same failure end to end through
the capture backend. Measured pre-fix: essentially the ENTIRE recording is
silence when the loop starts within half a second of the wrap (a linear
readahead, seeded there, has never looked at `loopStart_` by the time the wrap
arrives) — the case's own header records the exact numbers.

**The fix is TWO WINDOWS, not one.** While cycling, each tick splits its
`pagesNeeded` budget at `loopEnd_`:

* **Range A** — the rest of THIS pass, up to `loopEnd_`. Uses the SAME state
  as non-cycling playback (`readaheadPrevPage_`, `readaheadComputedUpTo_`,
  `pendingDemand_`), so a pass that never reaches the wrap on a given tick is
  indistinguishable from ordinary linear playback — and the non-cycling code
  path itself is UNTOUCHED (a separate `if (!loopValidNow)` branch, byte-for-
  byte the pre-fix loop), which is what makes non-cycle playback byte-
  identical (H7) a matter of construction, not measurement.
* **Range B** — the LOOP-START pages, pre-fetched AHEAD of the wrap once range
  A's own window for this tick settled cleanly. A SEPARATE chain
  (`readaheadPostWrapPrevPage_`, `readaheadPostWrapComputedUpTo_`,
  `pendingDemand2_`/`pendingDemandStart2_`/`pendingDemandEnd2_`/
  `pendingDemandEpoch2_`) — mirrored, not shared, because the two windows are
  not contiguous with each other and a shared frontier could not describe
  "how far into THIS pass" and "how far pre-fetched into the NEXT one" at the
  same time.

**The jump detector must tell a WRAP from a SEEK**, or the pre-fetch is
pointless: a wrap lands exactly on the loop-start page and moves backward,
which is indistinguishable from a real backward seek by position alone. The
distinguishing signal is `readaheadPostWrapComputedUpTo_ > pageStart` —
something has actually been pre-fetched for a pass that has not started yet.
When true, the frontier is ADOPTED (`readaheadComputedUpTo_ =
readaheadPostWrapComputedUpTo_`, `readaheadPrevPage_ = readaheadPostWrapPrevPage_`)
instead of being collapsed to `pageStart` the way a real jump still is (H5,
unchanged for an actual seek). Either way `readaheadPostWrapComputedUpTo_` and
its chain are CONSUMED (reset to empty) — a fresh pre-fetch is earned for
whichever wrap comes NEXT, never carried over from the one that just happened.

**`AudioEngine::startPlayback()`'s buffering check had to become loop-aware
too, and this was NOT anticipated going in — it was found by a qxa case
seeded away from `loopStart_` reporting ZERO captured frames for the WHOLE
run, not merely a gap at the wrap.** The check is `readaheadComputedUpTo_ >=
playPos + minBufferFrames_` (144000 frames, ~3 s) — a LINEAR difference. Once
range A caps `readaheadComputedUpTo_` near `loopEnd_` (the whole point of not
wasting the window past it), that difference can UNDERSHOOT the threshold
FOREVER whenever the loop is shorter than `minBufferFrames_` and the playhead
is not close to `loopStart_` — the readahead can have frozen everything there
is to freeze for the pass and still never satisfy a check written for
unbounded linear growth, so `startPlayback()` never returns PLAYING and the
device callback never truly starts advancing the playhead. Fixed by making the
"how much is buffered" measure loop-aware exactly where the demand side
already is: `min(readaheadComputedUpTo_, loopEnd_) - playPos` (coverage within
THIS pass) PLUS `readaheadPostWrapComputedUpTo_ - loopStart_` (coverage
pre-fetched for the NEXT one), summed and compared against `minBufferFrames_`
in place of the old linear difference. Non-cycling playback reads the
identical linear difference it always did (the loop-aware branch is gated on
`cycleEnabled_`), so this is a pure addition, not a rewrite of the existing
check.

**`readaheadCv_.notify_one()` was missing from two of `pullBlock()`'s
underrun/miss exits** (the resample path's readahead-gap `break` and the
passthrough path's "serious underrun" branch) — every OTHER miss path already
notifies, and these two silently did not, so recovery from exactly those two
exits waited for the readahead's next unconditional 20 ms tick instead of
being woken immediately. Both now notify, matching every sibling exit.

**The wrapped position is now published to `currentPos_` AT the wrap**, not
only after a successful batch. The readahead thread learns the playhead moved
by polling `currentPos_`; before this fix, a wrap whose first post-wrap page
missed left `currentPos_` at its pre-wrap (`>= loopEnd_`) value, so the
readahead kept demanding pages past `loopEnd_` that could never play and never
learned the wrap had happened at all.

Gates: `playback_test.cc`'s "Cycle wrap ACROSS MULTIPLE PAGES" case (watched
failing pre-fix: 151552 of 163840 frames silent/short, reliably, across
repeated runs; post-fix: 0 of 163840, every run) and the qxa case
`playback_loop_wrap_continuity` (watched failing pre-fix — first with the
locator at 0, which coincidentally warms `loopStart_`'s pages "for free" via
`minBufferFrames_`'s own lookahead and proves NOTHING either way, a trap this
gate's own header records so it is not rediscovered; then, seeded 20000 frames
short of the wrap, essentially the WHOLE recording silent). Measured post-fix:
the FIRST wrap costs 4908 frames (the one pass whose pre-fetch could not have
started early); every wrap after that is gap-free.

**A genuinely separate, PRE-EXISTING gap this fix also closed in passing**:
`SApplication::setPlaybackRunning()` (main/shell/CONTRACT.md's own entry) did
not re-sync the speaker's cycle state from the project before starting
output — only `SMainWindow`'s GUI Play handler did, via
`syncCyclePlayback()`, immediately before its own call to `startOutput()`.
Every PROGRAMMATIC transport start (`toggle-playback`, `record-start`, the
count-in/pre-roll preamble) went through `setPlaybackRunning()` alone, so
`cycle-enable` followed by a testkit `toggle-playback` left the speaker's
cycle atomics at their construction-time `false`/0/0 whenever the property
change fired before the speaker was ever asked about — which a headless
`--test-case` run hits on the very first `cycle-enable` of the process. A
real Play BUTTON click was never affected. Found only because
`playback_loop_wrap_continuity.qxa` could not get `AudioEngine::cycleEnabled_`
to read true AT ALL through `toggle-playback`, by ANY property-setting order —
confirmed by instrumenting `twSpeaker::setCycle()` directly and observing it
was never called.

**NOT fully verified**: the LEGACY synchronous readahead path (no
`CaptureRevalidator` scheduler, `SMARAGD_REVAL_WORKERS=0`) shares the SAME
range-A/range-B code (`tryPage`'s non-scheduler branch) and is what
`playback_test.cc`'s C++ harness exercises directly — that half is gated.
Production and every qxa gate run with the scheduler attached
(`twSpeaker::startOutput()` calls `engine->setScheduler(pageScheduler_)`
unconditionally), which is the path `qxa.playback_loop_wrap_continuity`
exercises end to end. Real device latency, jitter, and a loop shorter than
one page combined with heavy concurrent scheduler load are not covered by
either gate.
