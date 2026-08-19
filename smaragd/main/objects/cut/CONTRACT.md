# app/objects/cut — CONTRACT

Purpose: the AUDIO clip window object. SCut (startOffset/duration/loopLength/
grain window over any content, reader-chain ownership, container capture,
snapshot-based audio access), its inline renderer, and the window actions:
split-clip, unsplit-clip, resize-clip, duplicate-clip, set-pitch.

Public headers: app/objects/cut/*.h

Depends on (engine): tw/core, tw/graph, tw/pages, tw/schedule, tw/sources.
App edges: per tools/check_layering.py.

Invariants (normative detail: CLIP_MODEL.md, POSITION_DOMAINS.md):
1. Audio threads read via getSnapshot() only; window params change under
   mutex() then invalidate (drag path queues events, applies after).
2. mapTimelineToComponentPos mirrors seekTo exactly: both consume the ONE
   shared map, SCut::clipToReaderMap (proposal 18 Phase 4) — identity for
   looping, +startOffset otherwise (NO stretch scaling, see invariant 4).
   ensureReader() runs first. The preview consumes clipToSourceMap the
   same way; never hand-roll a second mapping.
3. rebuildReader is chain-descriptor-checked: plain trim/slip REUSES the
   reader; grain/loop changes mint a new one (fresh page cache).
4. The slip anchor is stored SOURCE-authoritative (proposal 18 Phase 3):
   srcStart_ is an exact Fraction, invariant under stretch edits; the
   warped-domain startOffset is DERIVED (floor(srcStart * stretch), the
   single render-boundary rounding). stretch is an exact Fraction born as
   a ratio of integer frame counts, denominator-capped at creation.
   cutDuration_/loopLength_ are integer timeline lengths. Positions are
   domain-typed (twdomains.h). Split arithmetic is exact rational and
   relies on the second cut inheriting grain params RAW (setGrainParams
   would preserve-span-rescale the duration).
5. Container-backed cuts capture via freezePage of the content root;
   arrangementChanged drops the capture transparently. A GRAINED cut's
   capture bakes the grain params in, so every grain-param setter must
   invalidateCapture() or the waveform preview keeps drawing the previous
   transform (playback is unaffected - it grains the raw source).
   THE CAPTURE IS AS WIDE AS WHAT IT CAPTURED (proposal 36 B7): the
   container branch copies EVERY channel of each frozen page into its own
   plane, through twPageClampChannel against the page in hand rather than
   the declared width, and the grained branch reads one plane per channel
   at the SAME offset (a grain source is random-access, so there is no
   cursor to displace — the per-channel loop §4.3 forbids inside a
   component's render is exactly right here). Width comes from the CONTENT,
   never from the project: a mono container yields a mono capture, and
   twTrackMix's §4.4 clamp spreads it. This is the only clip shape whose
   playback runs through twCapturingSource at all — rebuildReader calls
   buildCapture_ only when there is no random source, so a sample-backed
   stretched/pitched clip's capture serves PREVIEW only (proposal 36 §2
   item 2, corrected by B3).
5a. ensureCapturePeaks() folds EVERY CHANNEL of the capture into ONE signed
   envelope per probe (proposal 36 B8) — the smallest min and the largest
   max across the channels — matching SObject::straightCalcPreviewData. It
   read channel 0 alone, which drew the wrong waveform for any clip whose
   loud material is not on channel 0. The drawn waveform stays one lane by
   design; per-channel LEVEL is the meter's job, not the waveform's.
   SCut::getPreview reads capPeaks_, NEVER the aspect page's payload: it
   uses getPreviewCapture() only as a readiness signal (see
   app/model/CONTRACT.md inv. 9b, proposal 36 trap 26).
6. A SLIP-ONLY edit (setStartOffset / setSrcStart / setLoopStart) must
   invalidateRenderPathRange(0, duration). Nothing else notices it: the
   clip's position and length do not change, so twTrackMix::updateClip is
   never reached, while resolveClip folds the new slip into the very next
   freeze — already-frozen track/chain/mixer pages then keep the PRE-slip
   material at the same timeline position (mixed generations). Reader
   pages are deliberately NOT bumped (POSITION_DOMAINS rule 4: they are
   source-keyed, so a slipped clip just asks for different ones). The
   duration for the range comes from the SAME locked section that applied
   the edit, never a later getDuration() — a stale 0 makes the range
   empty and invalidates nothing. THROTTLED to ~3/s per cut (the live
   drag calls setStartOffset per mouse-move and each call walks the tree
   from the root): the first call after a quiet period is immediate, so
   single programmatic edits stay deterministic, and a coalesced one arms
   a single-shot QTimer so the final drag position always lands. Test:
   slip_invalidates_render_path.qxa, via the slip-clip testkit verb —
   resize-clip cannot cover this (it commits through setWindow, whose
   durationChanged stales the extent regardless).
8. SCut IMPLEMENTS `SClipWindow` (app/model/sclipwindow.h, proposal 37 D8b)
   and registers itself as the Audio wrap factory, so
   `SClipWindow::wrapContent(project, content)` mints a cut for audio content
   without anything in app/model naming SCut. THE RULE FOR THIS SLICE:
   **a windowed verb never casts to SCut for arithmetic the interface
   provides.** split / resize / duplicate / unsplit / set-clip-name / the
   take verbs / place-clip all go through `SClipWindow` — reads in timeline
   frames, `timelineToSourceExact` for the one map they need,
   `cloneWindowOver` for a faithful copy, `setWindowExact` to narrow it.
   A cast is legitimate ONLY for what is genuinely audio: pitch
   (set-pitch, remove-take's inverse), formant preservation, warp anchors
   (resize-clip, warp-marker actions), the grain params (remove-sample's
   inverse) and slip's throttled invalidation (the slip-clip test verb).
   Every one of those sites says so in a comment; a new one needs the same
   justification. Gate: the AC5 grep in proposal 37 P0a.
7. Pitch is stored in CENTS on twGrainParams, per clip and (on a stack)
   PER TAKE - only length ops write through to all lanes. It is realised
   in the grain stage (the read rate inside each grain) and is therefore
   duration-invariant: a pitch edit changes no window value, no position
   map and no clip edge. Every entry point clamps to
   SCut::PITCH_CENTS_LIMIT (+/-2400).

Gesture-driven quantities are clamped BEFORE they size an allocation.
cutDuration goes NEGATIVE mid-drag (right edge dragged past the left one) and
a far slip inflates windowEnd; buildCapture_() runs on a REVALIDATOR WORKER,
where a std::length_error out of buf.resize((size_t) negative) is
std::terminate, not a failed edit. need/dur/wantFrames/toRead are floored at
0 and the container capture is capped at the content's own duration (a window
past the source end reads silence anyway). Test: stress_edge_negdur.qxa.

capture_ is read through SCut::captureSnapshot(), never directly, from any
thread that does not already hold mutex(). It is published by buildCapture_()
(worker or render thread) and reset by invalidateCapture() (UI), so a bare
read is a shared_ptr data race; the snapshot also keeps the source alive for
the caller's whole use. captureBuildMutex_ does NOT substitute — it serializes
builders, while invalidateCapture() resets under mutex(). Tests:
stress_delete_churn.qxa, stress_stretch_split_slip.qxa (both swept over
SMARAGD_REVAL_WORKERS).

Self-registration (Phase 5): scut.cpp registers "SCut" with
SProjectLoader from a static initializer.

How to test: render_split_slip_offset.qxa (THE regression),
render_sawtooth_clipped_section.qxa, grain_*.qxa (the grain_pitch_*.qxa
cases assert the rendered f0 via assert-audio-frequency - energy alone
cannot see a transposition), mc_capture_clip_width.qxa (inv. 5's width half:
a stretched, a pitched and a container/asset clip over the 6 dB stereo ladder
of tests/test_stereo.wav, asserted at the clip through the real scheduler AND
in the rendered file — test_sawtooth.wav cannot gate a channel claim, its two
channels are byte-identical).

Known debt: loop tiling of container captures deferred; FIXME bounds check
in seekTo.

## Take stacks (proposal 17, phase 1)

STakeStack (stakestack.h) is the column of parallel takes — see the "Take
stacks" section of CLIP_MODEL.md for the model rules. Verbs: add-take
(wraps a plain cut on first use), remove-take (collapses at 1 take),
select-take; split-clip/resize-clip/unsplit are stack-aware (length ops
write through to every take, slip and pitch target one take via `take`).

Notes:
- A stack of EVENT takes forwards `resolveEventClip()` to its active take,
  exactly as the audio delegation forwards the component and the position map.
  Without it a MIDI column would be routed into the track's event clip set
  (its `contentKind()` says Event) and answer with an empty record — a
  silently mute column.
- A stack is a column of `SClipWindow`s, not of SCuts (proposal 37 D8b):
  `takeAt(i)` returns the window, `takeObjectAt(i)` the model object it also
  is, and `activeTakeObject()` is what the delegation (component, position
  map, preview, renderer) goes through. It is HOMOGENEOUS: `insertTake`
  refuses a window whose `contentKind()` differs from the takes already
  there — a take is an ALTERNATIVE for one region, so a column that played
  audio or notes depending on which lane is active would be a different
  feature. add-take turns that refusal into a rejected action; the loader
  turns it into one skipped take and keeps the column.
- The stack serves a private silent component while no take is active
  (STakeSilence in stakestack.cpp) — objects/cut may not include tw/mix,
  so no twRewire here.
- Wrap/collapse preserve the lane child index (moveChildToIndex) so
  recorded action paths and inverses stay valid — do not "simplify" this
  away.
- Undo fidelity limit: removing a take whose content is not file-backed
  (container asset) is applied but NOT undoable (inverse = null); a
  collapse triggered with activeTake == -1 resurfaces the remaining take
  audible (plain cuts cannot be inaudible).

9. **The clip GAIN envelope is drawn by the inline renderer, after
   `drawWarpMarkers`** (proposal 37 P6, design 6.1). A `cut:Gain` lane lives on
   the WINDOW and travels with it across placements and takes, so a lane of its
   own on the track would be lying about what it belongs to. Nothing is drawn
   when the cut has no envelope, which is what keeps every existing clip
   looking exactly as it did. The curve is sampled per pixel through
   `SAutomationLane::valueAt` — the same call the assertions make — over its
   own [0, 1] linear-amplitude axis with unity at the top. The GESTURES for it
   are not here: hit-testing is the arranger's business (app/timeline), and
   objects/cut may not depend on it.

How to test: takes_comping.qxa (audibility, comping per column, undo),
takes_serialize_roundtrip.qxa (loader registration, per-column activeTake
persistence incl. -1).

Phase 2 verbs (recording): place-clip (path-addressed windowed plain-cut
placement; inverse SUnplaceClipAction), place-recording (plans the file
span against the lane's columns — takes for covered columns, place-clips
for gaps, straddling columns untouched — applied atomically via
SCompositeAction from app/actions). Test:
takes_recording_placement.qxa.

Phase 4 (edit groups): split-clip/resize-clip/select-take (and move-clip in
objects/track) carry a `broadcast` attribute — a grouped anchor
(SObject::editGroup, helpers in app/model/seditgroups.h) fans out to every
member's corresponding clip (positional: same startTime+duration) as one
SCompositeAction; fan-out children carry broadcast=0. select-take comps the
same take INDEX; resize syncs the slip to the corresponding take (an
active-take anchor resolves its index first). Test:
takes_group_broadcast.qxa.

## The clip gain envelope (proposal 37 P5, design D5 / §3.3)

**`cut:Gain` is an automation lane on the WINDOW, and it therefore travels with
the window.** A `cut:` lane is stored in CLIP-RELATIVE frames on the `SCut`, so
moving the clip moves the fade, `cloneWindowOver()` copies it (which is what
makes `duplicate-clip` and `add-take` carry it), and a take stack's INACTIVE
take keeps its own — each take is its own window object and the mix reads the
active one. `SObject::copyAutomationFrom()` is the one copier; call it from any
future `cloneWindowOver()`.

The VALUE IS A LINEAR AMPLITUDE FACTOR (1.0 = unity), not dB, so a fade can
reach EXACTLY zero — which is what a fade-out is. The consumer is
`twTrackMix::freezePage_nolock`, which applies it to the child's page before
`mixFrom` (see `tw303a/mix/CONTRACT.md` inv. 23).

PLACEMENT-SCOPE envelopes (per-`SLink`) are deliberately deferred until proposal
32 gives links identity: an `SLink` has no id to hang one on today.

## Per-clip static volume and pan (per-clip volume/pan proposal, item f)

**Volume and pan reuse the generic `SObject::volume_`/`pan_` fields** — every
`SObject`, including every `SCut`, has already carried these (getter/setter,
signals, `Q_PROPERTY`, unconditional serialization) since long before this
slice; only `STrack` gave them meaning (its fader, in `twGainStage`). Nothing
here adds new storage — `set-clip-volume` / `set-clip-pan`
(`ssetclipvolumeaction.{h,cc}`, `ssetclippanaction.{h,cc}`) just give a clip's
existing fields an undoable entry point, mirroring `set-pitch` /
`set-formant-preserve` exactly (ABSOLUTE value, per-TAKE on a stack, edit-group
`broadcast`). Backward compatibility is therefore automatic: every project
file already round-trips a `volume='0' pan='0'` attribute on every `SCut`
element (`SObject::serializeSelfAttributes`/`readPreChildrenAttributes`), so
there is no format version to bump and no old-file default to invent.

**Volume IS applied in the audio path; pan is NOT.** `SCut::setVolume()`
override wires the clip's own `volumeChanged` to `invalidateRenderPathRange`
(self-connected in the ctor, the same idiom `STrack` uses for its own fader —
`volumeChanged -> onTrackVolumeChanged`), which reaches
`STrack::refreshClipGainCurves()` through the existing
`bumpRenderChainEpochRange` funnel — the SAME funnel a `cut:Gain` automation
edit already drives. That function now pushes BOTH the automation curve and a
linear `gainScalar` (the dB value converted once) into
`twTrackMix::ClipEntry`, and the mix loop multiplies the two per frame — see
`tw303a/mix/CONTRACT.md` inv. 26. `SObject::setPan()` is called directly with
no override and no invalidation: nothing downstream reads a clip's pan, by
design (see `tw303a/mix/CONTRACT.md` "Known debt").

**`cloneWindowOver()` copies both explicitly.** They are per-take properties
like pitch/formant, but unlike those two they are not part of `twGrainParams`,
so `setGrainParamsRaw()` does not carry them — without an explicit
`setVolume()`/`setPan()` call, split-clip/duplicate-clip/add-take would
silently reset a trimmed or panned clip back to unity on every copy.

## A cut over a LIVE RECORDING (proposal 21 L3b)

`SCut::isLiveRecording()` forwards its content's answer, so the predicate holds
for the window the arranger actually owns and not only for the content behind
it. Three paths are then SHORT-CIRCUITED, and all three for the same reason —
there is nothing derivable to cache from a source that grows ten times a
second, and building it is not free:

- `buildCapture_()` returns immediately. A capture is a RENDER of the content
  into a fixed-size snapshot; over a multi-second take that cost SECONDS on the
  UI thread and then crashed. This was the actual defect, found by
  `record_punch.qxa`: a `previewNonEmpty` assertion reached `getPreviewCapture`
  and the process stalled ~2 s and segfaulted.
- `ensureReader()` returns immediately. Building a grain/vocoder chain over a
  source whose length changes under it is both expensive and meaningless.
- `invalidateAspects()` returns immediately. Measured, a growing clip
  re-scheduling a Preview recompute per 100 ms tick starved the capture
  bridge's own thread badly enough to lose 2.2 s of input to ring overruns and
  put the capture backend 2.5 s behind its deadline.
- `getPreview()` delegates straight to the content, which answers from its own
  incrementally-extended peak ladder (`objects/wave` CONTRACT R4).

The WAV-backed cut that `place-recording` builds at stop is an ordinary one and
takes every one of those paths normally.

`place-recording` gained `srcOffset` / `length` (both defaulting to "the whole
file", so every existing call and script is unchanged). They place a SUB-RANGE,
which is what makes LOOP RECORDING one call per pass with no new machinery: all
passes go at the loop start, and this verb's own planner turns pass 2 onto pass
1's column as a take.

`STakeStack` gained the rest of the generic take-column seam (proposal 21 L4):
`windowTakeCount` / `activeWindowTakeIndex` / `insertWindowTake` /
`removeWindowTake` / `setActiveWindowTake`, and `stakehelpers.cpp` registers
`wrapCutLinkIntoStack` / `collapseSingleTakeStack` with
`SClipWindow::registerTakeColumnFactory` from a static initializer. Every
override is a ONE-LINE FORWARDER — the stack has been window-typed since
proposal 37 D8b and both helpers already took an `SClipWindow` — and none of it
changes behaviour here. It exists so `objects/midi`, which sits at THIS SLICE's
rank and must not depend on it, can build a column of EVENT takes
(`add-midi-take`). The homogeneity rule is what keeps that safe: `insertTake`
still refuses a window whose `contentKind()` differs from the takes already in
the column, so no column can end up playing audio or notes depending on which
lane is active.
