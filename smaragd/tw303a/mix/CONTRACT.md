# tw/mix — CONTRACT

Purpose: arrangement composition — twTrackMix (clips on a timeline, THE
consumer of the clip model), twMixer (bus summing), twGainStage (the track
fader), twRewire (fan-out).

Public headers: twtrackmix.h, twmixer.h, twgainstage.h, twrewire.h.

Depends on: tw/core, tw/pages, tw/graph. Forbidden: tw/sources (the mixer
sees only components via twView), app headers.

Invariants (normative detail in CLIP_MODEL.md and POSITION_DOMAINS.md):
1. ClipEntry identity is the opaque caller key (the app passes the SLink*);
   NEVER match clips by component pointer.
2. twTrackMix hands clips CLIP-RELATIVE positions; twView's MapPosFn does
   the domain translation.
3. freezePage_nolock clamps mixed child pages to the clip end
   (framesToMix = min(validFrames, clipEnd - mixStart)) — frozen pages carry
   full pages of material.
4. clip.previousPage chains per-clip DSP state across track pages.
5. seekTo_nolock seeks ALL clips (not just nearby ones) so no stale cursors
   survive a jump.
6. freezePage CLAMPS the requested length to twOutputPage::FRAME_CAPACITY.
   Unlike the base twComponent::freezePage_nolock — which ignores inputLength
   and always renders a full page — twTrackMix takes it at face value for both
   the page fill and the endPos of the clip-overlap walk, so an over-long
   length is a buffer overrun AND drags every clip in the track into one
   page's mix. Callers do size it from content: SCut::buildCapture_ passes the
   whole remaining capture length. inputLength == 0 means "no input data
   supplied" (RenderSession pulls the graph root that way), never "render
   nothing" — it maps to a full page.

7. ALL THREE COMPONENTS ARE N CHANNELS WIDE, AND N IS setChannels()'s
   (proposal 36 B4). A track is ONE twTrackMix and the master ONE twMixer; the
   N-parallel-width-1 arrangement they replaced is gone, and with it the idea
   that a "bus" and a "channel" are the same thing.
   - twTrackMix::freezePage allocates its OWN page and therefore BYPASSES the
     width wiring in twComponent::freezePage — it must pass getOutputChannels()
     by hand (§7 trap 19). Its clip mix is a LOOP over the page's channels, one
     IOVector per channel, and the SOURCE channel is twPageClampChannel'd: a
     clip's page carries the width of ITS source, so a mono file plays on every
     channel (§4.4). That clamp is exactly what the retired N-mixer arrangement
     achieved by rendering the same channel-0 page into each bus.
   - twMixer's INPUTS are tracks and its CHANNELS are channels. renderPageWide()
     accumulates `out[c] += src[clamp(c)] * factor` over the inputs in ASCENDING
     index order into a zeroed buffer — the same order and precision as the
     readStreamingData path it replaces, which is what keeps channel 0
     byte-identical while the graph widens underneath it.
   - twRewire is the channel-mapping component (`setChannelMap`): output channel
     c is input channel map[c], identity by default, clamped against the page in
     hand. A WIDE rewire is SINGLE-PLUG — that is the collapse — while the
     N-plug patch bay is untouched at width 1.
   - setChannels() bumps the content epoch. That is not what makes a runtime
     width change safe (§4.5 already reads an old-width page as a miss); it is
     what makes it converge instead of serving silence until some other edit
     invalidates.

8. TWGAINSTAGE IS THE TRACK FADER, AND IT IS POST-FX (proposal 37 P3a / D5).
   A track's chain is `twTrackMix -> twPluginChain -> twGainStage -> twRewire`,
   so the scalar is applied AFTER the inserts and BEFORE the track's root. The
   fader used to be `twTrackMix::trackGainDb_`, i.e. PRE-FX; that setter is now
   a NO-OP kept only until P5 deletes it and the field together, and the
   `factor != 1.0` guards it fed can therefore never fire.
   - The move is not observable to a LINEAR insert (a gain and a fader commute),
     which is why it is gated by a CLIPPER rather than by a byte compare:
     qxa.fader_post_fx case (b). Case (a) pins what must NOT change.
   - It implements BOTH render paths: renderPageWide() (the authoritative wide
     render, one upstream page in one pass, inv. 7's rule) and calcOutputTo(),
     which is both the legacy streaming pull and — through the base
     renderFrames() — the width-1 render. `page x gain`, nothing else.
   - CLASS INFINITY, PURE: a frame's output is a function of that frame's input,
     the scalar, and the frame's POSITION (the mute ramp's only state is an
     anchor). So reset() is empty and range invalidation over it is EXACT.
   - AT 0 dB, UNMUTED, IT DOES NO ARITHMETIC AT ALL — the render is a copy. That
     is what makes the committed golden corpus byte-identical across the move by
     construction rather than by luck (no golden combines a non-unity fader with
     a plugin; every `volume=` in tests/goldens/mc_{mono,stereo}.qxp is '0').
   - THE FADER'S EPOCH IS NOW THE ONE THE REWIRE'S PRODUCER CARRIES, which
     retires proposal 34's "the legacy pull does not observe a gain change made
     after a position was first frozen" caveat: setGainDb() bumps exactly the
     epoch twStreamingLatch::copyData gates the rewire's cached input page on.
     Gate: qxa.meter_gain_after_probe.
   - MUTE HERE IS THE AUDIO MUTE, ramped over ~1.5 ms from an anchor position,
     and P5's `self:Muted` automation lane is what will drive it. The mute
     BUTTON and the solo rules stay STRUCTURAL — the parent nulls the child's
     input plug (twMixer) or skips its clip entry (twTrackMix) — because mute is
     a property of the summing CHANNEL, never of a track's own output. Nothing
     in the app calls setMuted() yet.

Threading: clips_ mutations (insert/update/remove) are UI-thread under the
component mutex; render paths hold the same mutex during page assembly.
twGainStage guards its three parameters with a private mutex and snapshots them
ONCE per page into a local (THREADING rule 2), never re-reading mid-render.

How to test: `ctest -R mix_test` (clip windows, MapPosFn, clamp, key-based
update/remove against a scripted component — mix/tests/);
qxa.render_split_slip_offset and qxa.render_sawtooth_multiple_clips
end-to-end; for invariant 7, qxa.mc_track_width (distinct channels at the track
root, and the master equalling the per-channel sum of its tracks),
qxa.mc_width_change (2 -> 8 -> 2) and qxa.mc_legacy_pull_wide (the same with no
scheduler at all). For invariant 8, the twGainStage block of `mix_test`
(bit-exact unity, the exact scaled product, epoch staling, the mute ramp's
position-determinism, the width-1 path) plus qxa.fader_post_fx and
qxa.meter_gain_after_probe end-to-end.

Known debt: calcOutputTo allocates buffers per block. twTrackMix::setTrackGain
and trackGainDb_ are dead weight until proposal 37 P5 removes them.
twGainStage's mute ramp is implemented and unit-tested but UNWIRED — P5's
`self:Muted` lane is its caller. Pan does not reach the audio path anywhere
(a clip's `SObject::pan_` is modeled, serialized and editable — see item 26 —
but nothing downstream reads it; a full pan implementation needs channel
roles and a fold law, out of scope here).

## Automation (proposal 37 P5, design D5 / §4.5)

**`twGainStage` is THE fader, and since P5 it is also where a `self:Volume` and
a `self:Muted` automation lane are consumed.** `twTrackMix::setTrackGain()` and
`trackGainDb_` are **DELETED** — P3a forced them to 0 dB, P5 removed them, and
nothing in `twTrackMix` scales its own output any more. That is what makes a
track's frozen page its MATERIAL rather than its mix level, which in turn is why
an asset window over a faded track captures the unfaded audio.

19. **A CURVE IS A SNAPSHOT, SWAPPED, NEVER EDITED.**
    `setVolumeCurve(curve, absolute)` / `setMuteCurve(curve)` store a
    `shared_ptr<const twAutomationCurve>` under `paramMutex_`; `envelope()`
    reads each ONCE per page into a local (THREADING rule 2), so a page already
    being frozen finishes against the table it started with and a page frozen
    after the swap uses the new one. A swap bumps the content epoch, because the
    old curve is baked into every page already published.

20. **A NULL CURVE IS THE SCALAR PATH, and at 0 dB unmuted it is a PURE COPY
    with no arithmetic at all.** That is not an optimisation, it is the reason
    proposal 36's committed golden corpus is byte-identical across P5 by
    construction: no golden carries a lane, so no golden's samples are touched.
    `isFlat()` keeps the same property over the flat stretches of a STEP lane.

21. **TRIM SUMS IN dB; READ REPLACES.** `absolute == false` (Trim, the default —
    design §11 decision 3) evaluates `10^((gainDb + curve(pos))/20)`: a dB sum is
    a gain product, which is exactly "static value × curve". `absolute == true`
    (Read/Touch/Latch/Write) ignores the stored fader. The lane's VALUE DOMAIN is
    the fader's own dB (`app/timeline/sfadercurve.h`), and a `Linear` segment
    interpolates linearly IN dB — the design's "dB-linear in fader space" read as
    "linear in dB, in the fader's space", which is the only reading available to
    a module that may not include an app header.

22. **A MUTE LANE RAMPS AT EVERY TRANSITION, and it holds AUDIBLE before its
    first breakpoint.** `muteFactorFromCurve()` finds the last breakpoint at or
    below the position and ramps from the previous state over
    `muteRampFrames()` (~1.5 ms) starting AT that breakpoint — position-
    deterministic, so a page rendered out of order, twice, or on another thread
    produces the same samples and the component stays class infinity. Before the
    first breakpoint the answer is 1.0 (audible): "muted from frame 0" is what
    the STRUCTURAL mute says, and a lane drawn to mute a track at 1 s must not
    silence everything before it. The lane wins over `setMuted()`'s one-shot
    anchor, which remains for the button.

23. **THE PER-CLIP GAIN ENVELOPE (`cut:Gain`) IS APPLIED TO THE CHILD'S PAGE
    BEFORE `mixFrom`** — the "per-clip gain is not modeled" debt this file has
    carried since proposal 15. `ClipEntry::gainCurve` is a linear amplitude
    factor in CLIP-RELATIVE frames, swapped under `mutex()` by
    `setClipGainCurve(key, curve)` and read once per clip per page into a local.
    Output frame `destOffset + i` always corresponds to child frame
    `childPos + i` (both branches of the pair `freezePage_nolock` computes), so
    evaluating the curve at `childPos + i` makes it trim, slip and loop with the
    clip for free. It scales into a member SCRATCH buffer, never through
    `childPage`: that page is handed straight back as `clip.previousPage` (the
    child's DSP-state predecessor) and may be a page the child itself cached, so
    writing through it would corrupt both. The entry's `previousPage` is
    deliberately NOT dropped on a curve change — the envelope changes what is
    SUMMED, never the child's own state.

24. **THE FADER'S ARITHMETIC IS PUBLIC, AND IT IS THE ONLY COPY** (proposal 21
    L1a). `twGainStage::Envelope`, `envelope()`, `factorAt()`, `isFlat()` and
    `applyGain()` are public because the live pump applies a track's fader to a
    block it rendered itself, outside the frozen-page machinery, and it must be
    THE SAME arithmetic over THE SAME snapshot — otherwise an armed track's
    fader would differ from the frozen one it hands back to at disarm. Nothing
    else about the class is public, and the functions stay pure in position.

25. **THE MASTER IS A UNITY SUM WITH AN IDENTITY MAP, AND THAT IS A CHECKED
    PRECONDITION, NOT AN ASSUMPTION** (proposal 21 design D3). The live lane's
    "root(unarmed) + ring" split is legal only while

        master(unarmed ∪ live) == master(unarmed) + master(live)

    holds sample for sample, which needs `twMixer` summing at unity into a
    `twRewire` whose channel map is the identity, both at the project's width.
    `twlive::checkMasterShape(mixer, root, width)` (tw/playback) answers it over
    the two components; anything else — an insert, a non-unity input level, a
    re-map, a width disagreement — selects CLOSURE mode, in which the pump
    renders the master itself and the RT pops the ring only. `twMixer::
    inputLevel()` exists for exactly this check: a precondition that could not
    read the levels back would have to be assumed.
