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
10. **`SDuplicateClipAction::setCreatedPathOut()` (AC-a1) is an out-param
    written DURING `apply()`, never a getter read off the action afterward.**
    A caller building a Ctrl-drag macro (`app/timeline`'s
    `mouseReleaseEvent`) needs the new copy's path to select it, but the
    action can be `delete`d by `SActionHistory` the moment `submit()` returns
    on the REJECTED path — reading a member off the action pointer at that
    point would be a use-after-free. Writing into a caller-owned local during
    `apply()` (which only ever runs while the action is still alive) sidesteps
    the question entirely: the local simply stays empty when apply() fails.

11. **`resolveEventFeed()` NEVER NAMES A CONCRETE CONTENT TYPE, AND `objects/cut`
    HAS NO EDGE TO `objects/fragment`** (proposal 41 D4/D5/M3). `SCut`
    overrides only the base-class virtuals (`resolveEventFeed()`,
    `isPureEventContent()`, both on `SObject`) and reaches its content only
    through `getContent()` — a `SObject&`. This is what lets an `SCut` window
    a lane fragment's residual event feed without `objects/cut` depending on
    `objects/fragment` (see that module's CONTRACT.md — the dependency is
    CONCEPTUALLY cut -> fragment but is deliberately NOT a declared
    `check_layering.py` edge; M2's pack-clips/unpack-clips live in
    `objects/mixer` for the same reason). `resolveEventFeed()` applies OUR OWN
    slip/loop map over the content's already-flattened, content-relative
    sequence (mirroring `SMidiCut::resolveEventClip`'s window-over-content
    shape); it REFUSES (D5, never approximates) when `getStretchExact() != 1`
    on a cut whose content answers non-empty — the tick/frame conversion for
    that material already happened exactly once, inside the content's own
    window, and stretching here too would convert a second time in the frame
    domain. The edit surface (`SResizeClipAction`) refuses the EDIT itself
    with the same check, before `resolveEventFeed()` would ever be asked to
    log about it; the log line here is the belt to that suspenders for any
    other path that might set a non-unity rate.

    **D6's channel remap is deliberately NOT here.** It was tried on `SCut`
    first and is wrong: D2 shares ONE `SCut` across every placement of an
    asset ("edit any and all change"), so a remap stored on the content moves
    EVERY placement at once — the opposite of D6's own motivating case, one
    fragment placed on two tracks whose instruments want different channels.
    It lives on `SLink` (`app/model/slink.h`,
    `getEventChannelOverride()`/`setEventChannelOverride()`, -1 = as-authored)
    and is applied by `STrack::trackChildWasAdded`'s `resolveFn` closure —
    the one place that already holds both the SLink (the placement) and the
    resolved event sequence (the content) at once. `set-clip-event-channel`
    (`objects/cut/src/ssetclipeventchannelaction.cpp`) writes it there,
    scoped to an `SCut` placement by a `dynamic_cast` check, not because the
    storage needs `SCut` but because that is the shape this verb exists for.
12. **`isPureEventContent()` MIRRORS `isLiveRecording()` ONE FOR ONE** (proposal
    41 D7/M4): both are base-class `SObject` virtuals `SCut` forwards to
    `getContent()`, and both gate the SAME four call sites —
    `buildCapture_`, `ensureReader`, `invalidateAspects`, `getPreview` — for
    the same reason (a render/reader/cache/preview over content that can only
    ever be silent costs real UI-thread time for nothing). `getPreview`'s
    short-circuit returns `-1` directly rather than falling into
    `getContent().getPreview(...)` — a container's own preview path can reach
    `requestPage()`, a DEMANDED FREEZE forbidden on this thread
    (`main/timeline/CONTRACT.md` inv. 1).

How to test: takes_comping.qxa (audibility, comping per column, undo),
takes_serialize_roundtrip.qxa (loader registration, per-column activeTake
persistence incl. -1). Proposal 41 M3/M4: `fragment_midi_feed`,
`fragment_midi_no_double_trigger`, `fragment_midi_channel_remap`,
`fragment_midi_loop`, `fragment_rate_refused` (qxa, in
`objects/fragment`'s test list — the fixtures are fragment-shaped, the
assertions are on `SCut`'s behaviour).

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

**A COLUMN THAT STARTS BEFORE THE RECORDING IS DROPPED FROM THE PLAN, and
keeping it in silently threw the take away.** A column can only receive the
recording as a TAKE when it starts WITH it -- a take is an alternative for the
SAME window, so an earlier-starting column would need source material from
before the recording began. The plan loop duly skipped such a column, but the
collection loop still carried it into `columns`, and the plan loop advanced its
`cursor` past that column's END, so the column CONSUMED the recording material
it covered. Cover the whole recording and `cursor` reached `recEnd`, the
trailing-gap branch never fired, and the composite came out EMPTY -- and an
EMPTY COMPOSITE APPLIES AS SUCCESS, so the take was discarded with nothing but
a `qWarning` to show for it. Partial cover lost the take's HEAD the same way.

It is dropped at collection now, so the recording falls through to the same
trailing-gap branch an empty lane uses and is placed as its own clip,
OVERLAPPING the older one. That is deliberate: overlapping clips are what the
lane already holds whenever two takes do not line up, and losing recorded audio
is not something a recorder may do. The take-STACKING path is untouched -- a
recording starting exactly with a column still becomes a take on it.

This is what made `qxa.record_stays_armed` fail about 1 run in 15: its two takes
start a few thousand frames apart, because each record-start re-anchors its own
placement conversion, so whether take 2 begins just before or just after take 1
is wall-clock JITTER -- and only the second spelling lost the audio. Gated
deterministically by `qxa.place_recording_over_earlier_clip`, which drives the
verb directly with no capture device and no transport at all.
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

## A container capture carries the epoch it was built from (proposal 09 M0)

`SCut::captureContentEpoch_` records the content's render-chain epoch at the
moment `buildCapture_` starts freezing, and `ensureReader()` treats a capture
whose stamp no longer matches the content's current epoch as a **MISS** — it
drops it and rebuilds, rather than handing out the snapshot.

**This is not belt-and-braces over the existing invalidation; it closes a race
that invalidation cannot close on its own.** A container capture is rebuilt on a
revalidator worker, while the edit that should invalidate it bumps the content
epoch on the main thread. The two race, and the losing order is silent: a
rebuild that begins a few hundred microseconds before the bump lands reads the
**pre-edit** pages, publishes them, and sets `readerTried_` — after which
`ensureReader()` returns immediately and every subsequent render hears the stale
snapshot until some *later* edit happens to invalidate again. Measured at **6
failures in 12 runs** on an idle box, and racy at `SMARAGD_REVAL_WORKERS=1` as
well as 8, so it is not the worker pool.

Three consequences worth keeping:

- **The stamp is taken BEFORE the freeze loop, never after.** A content change
  that lands *during* a build then leaves the older value on the published
  capture, which is exactly what makes the next reader treat it as stale. Taking
  it afterwards would stamp a capture built from mixed content as current.
- **It is inert for leaf-backed cuts.** `contentEpochForCapture_()` returns 0
  when the content has a random source, and `0 == 0` compares equal, so
  sample-backed and grained captures — which are invalidated explicitly by their
  own window and grain edits, and have no container epoch to track — take
  exactly the path they always took.
- **A stamp, not an ordering.** The alternative (make the render wait for
  pending revalidation) forces a global barrier to fix a local staleness
  question, and would still be wrong the moment anything else rebuilt a capture.
  A mismatch that self-heals on next access is the same discipline proposal 36
  §4.5 applies to a cached page whose width no longer matches its producer.

Gate: `arrangement_edit_audible.qxa`, which is **probabilistic and says so** —
it runs four independent edit/render pairs to raise detection (one pair caught
the broken binary 4 times in 12; four pairs catch it 6 times in 12). 20/20 green
with the fix, and 5/5 at each of `SMARAGD_REVAL_WORKERS` 1/4/8/16. It cannot be
made deterministic without test-only sequencing hooks in production code.

### `windowStep()` is the NON-BLOCKING forward map (proposal 09 §15)

`SCut::windowStep()` maps one clip-relative position into the content — the
forward twin of `mapChildRangesToSelf`, and deliberately spelled the same way
(the same looping predicate, the same `twLoopMap` base, the same
`twWarpMap`) so the two cannot disagree about where the window maps.

**It takes `getSnapshot()` and never `ensureReader()`.** A repaint calls it
(the arrangement-tab playhead), and `ensureReader()` on a container-backed
asset builds a capture. The blocking siblings — `seekTo`,
`mapTimelineToComponentPos`, `resolveClip` — are freeze/edit-path calls and
stay as they are.

`STakeStack` overrides it to step into the **ACTIVE** take only. The base's
`childLinks()` fallback would descend into every take and report an inactive
one's material as audible.

## Preview READINESS is the CAPTURE, not the aspect page

`SCut::getPreview()` decides "is there anything to draw" on
`captureSnapshot() != nullptr`. It still CALLS `getPreviewCapture()` first,
for that call's side effect — `getCapture()` schedules a Preview revalidation
whenever the current page lacks the aspect, and the paint path is what drives
that scheduling — but it does not gate on the result.

**Why the page is the wrong signal.** `getCapture()` returns the current page
even when the requested aspects are MISSING ("stale is OK; better than
null/dropout"), so a non-null page says only that the revalidator has ever
published one for this cut — never that it describes the audio below. The data
`getPreview()` actually reads is `capPeaks_`, which `ensureCapturePeaks()`
builds from `captureSnapshot()`; and `invalidateCapture()` resets
`currentPage_`, `capture_` AND frees `capPeaks_` in one breath. The capture
therefore carries the identical invalidation guarantee, applied to the data
actually used — so this is strictly more correct, not a relaxation.

**What the old gate cost.** A container-backed cut — an ASSET, or the `SCut`
wrapping an `STakeStack` — painted as a solid body with no waveform whenever no
page had been published, with a complete capture sitting right there. Measured
headlessly after load + render + a 3 s settle: `page=0 snap=1`, with
`buildCapture_` having logged a 192000-frame container capture five times.
Nothing in a headless run ever publishes that page, so every asset clip was a
blank rectangle in every `.qxa` context and the composite lane's waveform could
not be gated at all. Gate: `asset_clip_preview.qxa`, watched failing.
