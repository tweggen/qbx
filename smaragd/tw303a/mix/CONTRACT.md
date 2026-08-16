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

Known debt: calcOutputTo allocates buffers per block; per-clip gain/pan not
yet modeled (track-level only). twTrackMix::setTrackGain and trackGainDb_ are
dead weight until proposal 37 P5 removes them. twGainStage's mute ramp is
implemented and unit-tested but UNWIRED — P5's `self:Muted` lane is its caller.
Pan does not exist.
