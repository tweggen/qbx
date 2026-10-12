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

### Take extents: invariant 1 became 1′ (plan 53 §4.1, QBX-177)

EVERY TAKE HAS AN EXTENT `[tStart, tEnd) ⊆ [0, duration_)`. A take covers only
the span its material actually occupies, and **there is no silent take body
anywhere**: a position no take covers plays SILENCE, and there is no fall-back
to another take that happens to cover it (plan 53 §3.1 step 5).

Before 1′ every take spanned the whole column and take-link `startTime` was
always 0. Mode A needs less than that: when several existing children are
folded into one union column, each becomes a take AT ITS OWN EXTENT, and the
alternative — inventing silent take bodies to fill the gaps — is what §3.1
forbids outright.

**Where it is stored.** `tStart` is the take `SLink`'s `startTime` and
`tEnd - tStart` is the take window's own duration. No new per-take field and no
new element: both were already written (`SLink::hasStartTime()` is
unconditionally true), the loader simply DISCARDED the start time by forcing
`setStartTime( 0 )` in `insertTake`. `STakeStack::insertTake( window, atIndex,
tStart, tLen )` takes them now, with defaults that mean "the whole column", so
every caller written before extents is correct unchanged.

**The column has a length of its own**: `STakeStack::duration_`. It did not
before — `getDuration()` answered the ACTIVE take's duration, else take 0's —
and that is exactly what a partial take makes untenable: W1's column, whose
active take covers only its first half, would report half its length and the
clip's right edge would move as the user clicked between takes. Both getters
return `duration_`, which also removes the stale-try-lock hazard that made
`getDurationBlocking()` necessary for a column at all. Serialized as
`duration=` on `<STakeStack>` **only when it differs from
`max_i( tStart_i + len_i )`** and derived as that maximum when absent — which
is every file written before `FORMAT_VERSION` 3, so they all re-serialize
byte-identically.

**THE RESOLUTION RULE — ONE PLACE HONOURS EXTENTS.** A column with ANY partial
take (`hasPartialTake()`, cached) resolves through `twCompColumn` even when its
map is EMPTY. The original path hands the column position straight to the
active take, which is correct only while that take is full-extent;
`twCompColumn` is the one object that knows how to shift a take's position by
its `tStart` and contribute nothing outside its extent. The DEGENERATE column
— no map, every take full — still takes the original path, byte for byte, for
proposal 43 N2's unchanged reason: an extra page copy is bit-exact, but routing
a stretched take's DSP state through a second component's chaining is not.

In the engine, `twCompColumn::setInputExtent( i, tStart, len )` carries it, and
three things follow from it: a run never straddles an extent edge (so plan and
freeze ask the same question in the same order, proposal 19 Inv-1), a take with
no material at a position **declares no page dependency** there (declaring one
would have the scheduler build a lazy reader for a window the take does not
have), and `clampedXfade_nolock` clamps a crossfade to BOTH takes' extents —
W1's boundary at `T` has no outgoing material past it, and a fade that ignored
that would fade down the only material there is.

**Invalidation forwards for EVERY take**, not only the active one.
`onTakeCutChanged` and `mapChildRangesToSelf` used to drop everything but the
active take, on the reasoning that only it reaches the mix. Under a comp map
that was already false (pre-existing since 43 N2, for hand-made comps), and
under 1′ it is false for every recording over existing material: a later slip,
volume or fade edit on an audible INACTIVE take would bump no epoch and
invalidate no range. The ranges are shifted by `tStart` — the base's ShiftMap
already does that, because `tStart` IS the take link's start time — and
intersected with the extent, so an edit outside a take's own span still bumps
nothing.

**The write-through CLAMPS rather than overwrites.** `setDurationAll` /
`applyWindowAll` move the column and then clamp each take into it: a trim
shortens every take that reaches past the new end, an extend grows only the
takes whose extent ENDED at the old end (they were clamped BY the column; one
that ended earlier was not, and must stay where the user put it). For a
full-extent column every take is in the second case, so this is exactly the old
"set every take's duration" — which is why no existing column gate moves. A
trim that would EMPTY a take is clamped to one frame and logged here; REFUSING
it needs a verb that can reject, and since A0b `resize-clip` does, before
anything is mutated and with a `resize-clip: REFUSED` warning. The two are not
alternatives: the clamp is the MODEL's floor, which keeps 1′ true whatever a
caller does, and the refusal is the EDIT SURFACE's answer. `resize-clip` also
stopped writing the column's length onto a take in either of the two branches
that used to — the named-take slip and the left-edge move — because
`applyWindowAll` has already clamped every extent correctly by the time they
run, so writing `duration_` there was invariant 1 still speaking and would
silently stretch every partial take back over the whole column.

**Invariant 3 became 3′**: a one-take column collapses to a plain cut only when
that take is FULL-EXTENT (`collapseSingleTakeStack` returns null otherwise,
which every caller already handles as "did not collapse"). A one-take column
with a PARTIAL take is legal — it is a clip with a hole in it, the union
placeholder of §4.2 — and its length is `duration_`, so it is representable.

**`split-clip` asks MEMBERSHIP first.** A take wholly on one side belongs to
that half only; a take wholly in the TAIL is MOVED there, same object, its
`tStart` shifted by `-inObjOffset` (plan 52's identity rule — a clone is not
the object the user's fades are on); a take CROSSING the split is trimmed in
the head and cloned into the tail, which is what the code always did. Indices
are renumbered per half and each half's map is RE-INDEXED onto its own
numbering (`stakes::remapCompMap`; a segment naming an absent take is dropped
and the previous one extends, the rule `removeTake` already applies). Each
half's active take is the original one when it has material there, else the
lowest-index take with material at the half's start — a half may not come out
with an active take that is silent everywhere in it. A half that would hold NO
take is REFUSED (3′), which is unreachable for a full-extent column because
every take crosses.

Its inverse is therefore MIXED: `SUnsplitClipAction` carries a `TakeRestore`
per pre-split take and walks them in ascending original index, moving the
tail-resident ones back by identity and restoring the rest by extent, before
the tail link (and with it the clones) dies. Which half a take came from is
precisely the information the split consumed, so only the splitting action can
hand it back — the same reason its comp map is an inverse-only payload.

**`remove-take` carries the extent in its inverse** (`add-take at=`/`length=`),
or undoing the removal of a partial take would give back a full-extent one. And
the column does NOT shrink: removing the take that reached furthest leaves a
span nothing covers, which plays silence rather than moving the clip's right
edge under the user.

**THE PICTURE HONOURS EXTENTS SINCE A0b** (plan 53 §4.3–4.4, QBX-178), and the
whole of it moved together for the reason A0a recorded: a preview is what a
painter reads, so windowing one alone would have produced a half-windowed
picture — an envelope that stops at the extent inside a waveform that does not.

- `hasPreview`/`getPreview` are the active take's, WINDOWED to its extent;
  outside it the probes are ZERO, which is how every consumer of that seam
  already spells "no material". `hasPreview` itself needs no extent test — an
  extent is at least one frame, so a take that exists always has some material.
- The COMPACT painter draws the COMP RESULT, run by run, instead of the active
  take across the whole column (plan 53's X10: the picture showed material the
  column does not play and hid material it does). A run whose take has no
  material is HATCHED, in the same brush the "no take" column uses, because the
  statement is the same one. The HATCH's pixels are QBX-157's gate; what A0b
  gates is that the span is silent and its envelope empty.
- The TAKE-LANE terminals (`drawTake`/`collectTakeEnvelope`) draw one take over
  ITS EXTENT only, and the lane's own region walk breaks at EXTENT edges as
  well as map boundaries (`nextCompEdgeAfter`), or a region would straddle the
  end of its own take's material.
- **A DEGENERATE COLUMN KEEPS THE ORIGINAL PATH** — no map, every take
  full-extent — exactly as the audio resolution does (`resolvesThroughComp`)
  and for the same reason: every project that has never been comped must look
  exactly as it did.
- Positions come from the FORWARD map only (`SRenderContext::getTimeOf`), per
  column, NEVER from inverting it. The pixel→frame map is affine for a plain or
  stretched clip but PIECEWISE for a warped one and per-tile for a looped one,
  so inverting it to find a segment's pixel would be wrong on exactly the clips
  comping is used on. The shift into a take's own domain goes in the CONTEXT
  (`TakeShiftContext`), not in a temporary `SLink`: the link is shared, and the
  take renderers read its loop, stretch and colours from it.
- `nextCompEdgeAfter` ignores CROSSFADES, deliberately. A crossfade is a BLEND
  that a per-column envelope cannot express; `twCompColumn` splits runs at its
  two ends because it has to MIX them, and the cut marks that would draw a
  boundary are QBX-157's.

Still unwindowed by the extent, deliberately: `resolveEventClip` (plan 53 P3,
MIDI parity — an event column needs the RECORD cut, not just its position
moved).

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

**Volume and pan are BOTH applied in the audio path** (pan since proposal 49
M1). The clip's own `volumeChanged` and `panChanged` are self-connected in the
ctor to `onVolumeChanged` / `onPanChanged`, each of which calls
`invalidateRenderPathRange` (the same idiom `STrack` uses for its own fader —
`volumeChanged -> onTrackVolumeChanged`). That reaches
`STrack::refreshClipGainCurves()` through the existing
`bumpRenderChainEpochRange` funnel — the SAME funnel a `cut:Gain` automation
edit already drives — which pushes the automation curve, a linear
`gainScalar`, the fade and the PAN into `twTrackMix::ClipEntry`; the mix loop
applies all four per frame (`tw303a/mix/CONTRACT.md`, "CLIP PAN IN THE CLIP
LOOP"). **`SObject::setPan()` invalidates nothing by itself**, which is why
`onPanChanged` exists (proposal 49 trap T2): without it a pan edit is
inaudible until an unrelated edit re-pulls the clip entry, and a gate that
renders only after the edit cannot tell. `set-clip-pan` REFUSES a non-zero
value on a project whose width is not 2 (D1), with a warning, and accepts 0
there so a reset never fails.

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

## A container CAPTURE is built through resolveClip(), never getRootComponent()

`buildCapture_`'s container branch renders the content the way PLAYBACK
resolves it:

```cpp
const twResolvedClip resolvedContent = c.resolveClip( 0 );
rootComp = resolvedContent.component;
pagePos  = resolvedContent.mappedPos;      // the buffer offset stays at 0
```

It used to be `c.getRootComponent()` frozen from position 0, and that dropped
the content's OWN WINDOW two independent ways:

* `getRootComponent()` returns the built READER when one exists and falls back
  to `content_->getRootComponent()` — the raw, UNWINDOWED content — when it does
  not. At capture-build time the inner clip's reader typically does not exist
  yet, and the capture is CACHED, so it LATCHED on first use.
* Even with the reader built it was still wrong: a plain reader is addressed in
  the WARPED domain with the slip folded in BY THE CALLER
  (`SCut::clipToReaderMap`), and freezing from 0 applies no mapping at all.

**This was an AUDIO defect, not a drawing one.** `rebuildReader` builds the
wrapper's playback reader over that same capture, so a `SCut -> STakeStack`
wrapper — what a SHARED or PLACED take column is — PLAYED the active take with
the take's own `srcStart` dropped. On the DIRECT shape (`SLink -> STakeStack`)
a take slip has always been audible (`take_lane_slip.qxa`), so the wrapped
shape was the anomaly.

`resolveClip()` is the one authority for both halves — proposal 19 Inv-1: ONE
structural snapshot yields the component AND the mapping — and it is what
`twView` asks on the playback path, so a capture is now built from exactly what
the engine would have played.

**The blast radius is small by construction.** `SObject`'s default
`resolveClip()` is `{ getRootComponent(), mapTimelineToComponentPos( off ) }`
with an IDENTITY map, and only `SCut` and `STakeStack` override either. A cut
over an `STrack` or an `SStdMixer` — an ordinary ASSET — therefore resolves to
exactly what the old line produced, base 0 included, which is why no golden
moved.

**`getRootComponent()`'s own fallback is unchanged and still a trap for a new
caller.** It is deliberately lazy (it must not build a reader on the UI thread),
so it answers "the raw content" whenever no reader exists. Anything that needs
what PLAYS must ask `resolveClip()`. Gate: `wrapped_take_window.qxa`.

## A cut over MISSING content builds no capture at all (2026-08-22)

`buildCapture_` returns immediately on `content.isMissing()`, before every other
test in it.

A MISSING placeholder (`main/objects/wave/CONTRACT.md`) has no random source, so
without this the very next line classifies it as CONTAINER-BACKED and the
function renders its whole declared duration into a capture — a full-length
buffer of ZEROS, built on the UI thread, once per clip on the absent sample.
Measured on a real project carrying three unreachable samples: **~17 s of load**,
all of it producing silence.

Nothing reads what it would have produced. With no capture, `rebuildReader`
builds no reader (`needCapture` is true, `capture_` stays null, and the chain
short-circuits) and the clip is SILENT — which is exactly what the capture would
have said, at whatever cost.

This is the same class of decision as `isLiveRecording()`'s four short-circuits
just above, and for the same reason: a container-shaped thing that is not a
container must be routed around the capture path, not through it.

## A take COLUMN is resolved through `stakes::columnOfLink`, on BOTH shapes

A take column reaches a lane two ways:

```
DIRECT    SLink -> STakeStack                 (what add-take builds)
WRAPPED   SLink -> SCut/SMidiCut -> STakeStack (a SHARED or PLACED column)
```

`stakes::columnOfLink( SLink* )` (`stakehelpers.h`) is the ONE spelling of
"which take column does this placement carry". It casts, else unwraps exactly
one window level through the GENERIC `SClipWindow` interface — not through
`SCut` — because a stack is homogeneous by `contentKind()` and an EVENT column
wrapped by an `SMidiCut` is the same question. `SMVActualView`'s
`takeStackOfLink` now calls it, so the take-lane UI and the take-lane actions
cannot drift apart again.

**They had drifted, and both halves of the drift were user-visible.** The UI
unwrapped and the actions did not, so on a wrapped column — which a real saved
project carries — `select-take` was REFUSED outright (a click on a take lane
did nothing at all) and `resize-clip` fell through to its generic
single-window branch and wrote the TAKE's window onto the WRAPPER.

**Which cast to use is still per-action.** Most of objects/cut's other
`dynamic_cast<STakeStack*>( &link->getSObject() )` sites ask a different
question — `set-clip-volume` on a wrapped column really does mean the WRAPPER —
and are deliberately unchanged. Only the two verbs that exist to serve the
take-lane UI ask exactly the column question.

## `resize-clip` with `take >= 0` writes TWO objects on a wrapped column

The discriminator is **`take_ >= 0`** ("this edit names ONE take"), never the
shape. With `take_ < 0` the action is byte-for-byte what it was: a direct stack
takes the write-through branch, a wrapper takes the generic single-window
branch (a composite-lane resize or slip of a wrapped column edits the WRAPPER,
and must keep doing exactly that).

With `take_ >= 0`:

| field | goes to |
|---|---|
| `startTime` | the LINK, as always |
| `duration` / `loopLength` / `stretch` | the object the LINK carries: the stack (`applyWindowAll`, invariant 1's write-through) on the direct shape, the WRAPPER's own window on the wrapped one |
| `srcStart` | the ADDRESSED TAKE only, in the take's OWN geometry |

On the direct shape that is identical to the previous code — the link's object
IS the column and `duration_` equals what the take already had by
`stakestack.h` invariant 1 — which is why `take_lane_slip.qxa` and
`takes_group_broadcast.qxa` cannot move.

On the wrapped shape a wrapper is a WINDOW INTO the column, so its duration and
the takes' are legitimately different numbers, and writing one onto the other
breaks invariant 1 in whichever direction it is done. The INVERSE reads
`oldAnchor` from the take and `oldDur`/`oldLoop`/`oldStretch` from the object
the geometry will be written to, which is what makes undo exact on both shapes
despite the split. An out-of-range `take_` leaves `takeWin` null: the geometry
is still applied and the anchor write is skipped — the same thing the direct
branch has always done for "no active take".

## A CONTENT-IDENTITY change must be published through the PLACEMENT

`stakes::publishColumnChange( SLink*, STakeStack* )`. A take switch, or a slip
of one take, changes which material the column produces WITHOUT touching the
placement's timeline extent.

On the DIRECT shape the TRACK is connected to the stack's own
`durationChanged`, and its slot is what actually does the work:
`twTrackMix::updateClip` — a CONTENT-EPOCH BUMP and a state-chain reset — plus
the range invalidation. **On the WRAPPED shape the link's object is the WINDOW,
and a window does not listen to its content's `durationChanged`: that emission
has no listener at all.** So the frozen track / chain / mixer pages keep serving
the old material and the edit is INAUDIBLE.

The wrapper therefore republishes its own, UNCHANGED duration.
`SClipWindow::setDurationFromTimeline` has no unchanged-value early-out, so
that reaches the track's slot exactly as the direct shape does.

**Dropping the wrapper's capture and staling its render path are NOT
substitutes.** Both were tried and measured: with `invalidateCapture()` +
`invalidateRenderPath()` and no republish, the next render still served the old
take.

## Preview READINESS is the CAPTURE, not the aspect page — and a build that
## RACED an invalidation must be DISCARDED

`SCut::buildCapture_` runs on a revalidator worker holding only
`captureBuildMutex_`, which deliberately does not exclude `invalidateCapture()`
(that resets under `mutex()`). A build takes tens of milliseconds, so an
invalidation landing mid-build was simply overwritten by that build's own
publish a moment later — and the stale capture then read as fresh, because
`ensureReader`'s `captureContentEpoch_` check compares a per-COMPONENT counter
that a take switch need not move.

MEASURED, on a wrapped take column: a worker entered `buildCapture_` and
resolved the stack at take 0; `select-take` set take 1 and invalidated in the
SAME millisecond; the worker published its take-0 capture 2 ms later; the next
render served it. The click "worked" and the audio did not.

`captureGen_` closes it. `invalidateCapture()` bumps it; `buildCapture_`
samples it ONCE before it reads any content and publishes only if it has not
moved. A discarded build costs one rebuild — which the very next
`ensureReader()` does anyway, since `capture_` is null and `readerTried_` false
— and publishing it costs correctness.

**A gate for anything on this path must render BEFORE the edit as well as
after**, in one process. A case that renders only afterwards finds the capture
cold and correct, and cannot see this class of defect at all.

## A take COLUMN belongs to exactly ONE placement

Two placements of one `STakeStack` share its `activeTake_`, so comping either
comps both — which is indistinguishable, from the outside, from the comping
gesture not working. `stakes::cloneColumn()` is therefore what
`duplicate-clip` and the arranger's "add link" use for a column: a new stack
holding a clone of every take's window (sharing the same CONTENT, as
`cloneWindowOver` does everywhere else) and the same active index.

Before proposal 42 M1 both produced a SHARED column, and `duplicate-clip` also
produced the WRAPPED shape while doing it: `SClipWindow::of()` is null for an
`STakeStack`, so it fell through to `wrapContent` and minted a window over the
same stack.

**Whoever wants the SHARE semantics wants an ASSET** — proposal 41's fragment,
where "one object, N placements, edit any and all change" is the stated
invariant (D2) rather than an accident of which verb was used.

Gate: `take_column_one_shape.qxa`, and the `assert-take-column` verb, which is
the only thing in the suite that can see a placement's take-column SHAPE, its
take count, its active take and its PLACEMENT COUNT. `assert-clip-window`
cannot: it reads the link's own object, so it cannot tell a column from a
window over one, and its `take=` attribute is silently ignored on the wrapped
shape.

## Splitting a WINDOW over a take column forks the column

`split-clip`'s generic window branch clones the window; for a take column that
is not enough, because two placements of one `STakeStack` share its
`activeTake_`. The head keeps the original column and the tail gets a deep copy
(`stakes::cloneColumn` + `SClipWindow::cloneWindowOverContent`); each window
already covers its own region of the column, so no take window has to move.

Before proposal 42 M3 both halves shared one column, and **per-region comping —
the entire point of splitting a take column — was impossible on the wrapped
shape**. Measured: comping the HEAD moved the TAIL's audio too. A model
assertion could not see it, because the split arithmetic was always right; only
a RENDER can, which is why the gate measures RMS.

The DIRECT shape keeps its own branch, which additionally TRIMS every take to
the split point. A wrapper must NOT do that: the wrapper does the windowing, and
trimming the takes as well would window twice.

`add-take` and `remove-take` now resolve through `stakes::columnOfLink` too.
DIRECT-only, `add-take` saw a wrapper as a plain clip and wrapped it AGAIN —
producing a COLUMN INSIDE A COLUMN, the new take a sibling of the entire old
column — and `remove-take` was simply rejected.

**Known, and left to M4:** `collapseSingleTakeStack` is DIRECT-only by
construction (it replaces a lane's child link), so a WRAPPED column that drops
to one take stays a one-take column rather than collapsing — `stakestack.h`
invariant 3 holds on the direct shape only. Collapsing a wrapped one means
composing the wrapper's window into the remaining take, which is exactly the
migration M4 does.

## A column EDGE edit keeps the takes aligned with each other

`resize-clip` with **no** take named is an edit to the COLUMN: the extent goes
to every take (invariant 1) and every take's anchor shifts by the SAME delta.
With a take named it is the take-lane SLIP and the anchor is that take's alone.

The anchor used to go to the active take alone in both cases. Measured,
trimming a column's left edge 12000 frames later: the inactive take's anchor
moved by 0 and the active take's by −12000 — **opposite directions, 24000
apart** — so switching takes afterwards gave different material at the same
instant. That is precisely the operation comping needs when a boundary is
moved, and it is why the desync was invisible until someone tried to comp.

## A saved WRAPPED column is normalised to the DIRECT shape on load

`stakes::normalizeColumns` (proposal 42 M4), registered as a post-load pass
from `stakehelpers.cpp`'s static initializer — the same self-registration the
loader's class registry and `SClipWindow::registerWrapFactory` use, and for
the same layering reason: `app/persistence` is `app_core` and may not name
`STakeStack`.

**Composition, not deletion.** Each take's anchor becomes the content position
the wrapper's anchor maps to THROUGH THAT TAKE'S OWN MAP
(`timelineToSourceExact`, so a stretched or warped take lands where the wrapper
actually showed it), each take takes the wrapper's duration, and the wrapper's
dB volume is SUMMED onto each take — which is how the mix already composes a
clip's static gain. The column object, its active index and every take's
identity survive, so nothing downstream sees a new object.

**Refused, not approximated**, when the fold is not exact: a wrapper that
loops, is stretched, is panned or carries an automation lane, or a take that
loops. Such a column keeps its wrapper, is counted, and is logged. M2 and M3
handle it correctly — it is simply not migrated, and that path is gated too.

`SMARAGD_TAKE_MIGRATE=off` disables the pass; the gates for the wrapped shape
itself run with it.

**Two things this cost, both found by measurement and both worth knowing:**

* **The loader must be DESTROYED before a post-load pass runs.** Its
  `objectDict_` holds one temporary handle `SLink` per loaded object, released
  only in `~SProjectLoader`, so while it is alive every object's reference
  count reads one too high. Measured: a wrapper the pass had just unlinked
  still reported 1 reference, so it was not deleted, survived into the saved
  file and kept the column at `nRefs='2'`. `SLoadProjectAction` now scopes the
  loader.
* **The collapsed wrapper is deleted EXPLICITLY, not by refcount.**
  `removeRef()` schedules a `deleteLater()`, and `SProject::serialize` writes
  every `SObject` still parented to the project — so a wrapper waiting on the
  event loop is written into the very file this pass exists to clean. Qt
  removes a pending `DeferredDelete` in `~QObject`, so an explicit delete after
  one is safe, and the refcount test is what makes it safe here: a wrapped
  column has exactly one placement (M1), and it is the link just deleted.

Gate: `take_column_migrate.qxa`, whose audio assertions are copied VERBATIM
from `wrapped_take_window.qxa` — the same fixture and the same four closed
forms, asserted once through the wrapped shape and once through the migrated
one, so a fold that moved the audio by a frame fails. Plus the refusal path
over `tests/takestack_wrap_looping.qxp`, which is that fixture with
`loopLength='48000'` on the wrapper and nothing else changed.

## The COMP MAP: which take of a column sounds WHERE

`twCompMap` (`tw/events/twcompmap.h`, beside `twAutomationCurve` and modelled
on it) is a sorted list of segments in the COLUMN's own frame domain — which is
NOT the placement's domain when a wrapper still windows the column, and
proposal 42 M4 deliberately leaves some of those in place. A segment's take
governs `[seg.at, next.at)`, the same left-point convention `twAutomationCurve`
uses; `xfade` is the crossfade CENTRED on the segment's own left boundary, so
it belongs to the BOUNDARY rather than to either take.

**THE EMPTY MAP IS THE DEGENERATE CASE AND IT IS LOAD-BEARING.** An empty map
means `activeTake_` everywhere. That is what makes every project written before
proposal 43 — and every golden, and the whole of proposal 42's gate set —
correct and unchanged: no map, no behaviour change, and `<comp>` is written
only when non-empty (the `<automation>` rule). `STakeStack::takeIndexAt()` is
THE one place the substitution is spelled, so no consumer has to remember it.

`normalized()` sorts, drops negative positions, clears the first segment's
crossfade, and REPLACES a segment at an existing position rather than keeping
the earlier one. That last is a stable fold and not `std::unique` on purpose:
`std::unique` keeps the FIRST of an equal run, which is exactly the bug
proposal 37 P6 found in `SAutomationLane::setPoints` — an edit landing on an
existing position silently DROPPED while the docs promised a replace. Clicking
a boundary you already made is the commonest comping gesture there is.

`setCompMap` publishes by the SAME route and in the same order as
`setActiveTake` — mutation, then `durationChanged` — so the track's slot runs
`updateClip` (a content-epoch bump and a state-chain reset) and the range
invalidation. N1 has no consumer, so that publishes a change nothing hears yet;
wiring it now is what means N2 adds a component and no plumbing.

**`select-take` CLEARS the map**, because it means "this take for the WHOLE
column", and its inverse carries the previous map back — one undo of a click
must not silently destroy a comp. `SSelectTakeAction::setPreviousCompMap()` has
exactly one caller and must keep exactly one, the same discipline
`SSetPluginParamAction::setPreviousValue()` carries.

Every comp verb's INVERSE is the WHOLE previous map. A per-segment inverse
would have to name a segment, and a segment's only identity is its position —
the very thing `move-comp-boundary` changes.

Gate: `comp_map_model.qxa`, whose last assertion is that a render is UNCHANGED
by the map — the one N2 will flip.

## A clip FADE is audio-only, and it is a third factor in the mix's product

`twClipFade` (`tw/events/twfade.h`, beside `twCompMap` and
`twAutomationCurve`): a fade-in length, a fade-out length and a SHAPE, in the
clip's OWN frame domain — so a fade trims, slips and loops with its clip,
exactly as a `cut:Gain` curve does.

**There was no fade primitive anywhere in this model before proposal 43 N5.**
The two things that look like one are not: `twGrainParams::crossfadeMs` is a
granular-synthesis knob, and the live monitor's 2-3 ms ramp is an RT detail of
the device lane.

It lives on `SCut`, not on `SClipWindow`: a fade is a GAIN shape, and that is
audio-only by the same rule pitch, formant preservation and warp anchors follow
(invariant 4). `set-clip-fade` REFUSES an event clip rather than ignoring it —
an event clip's equivalent is velocity, a different thing in a different
domain.

**THE FADE-OUT IS ANCHORED TO THE CLIP'S END**, which is why `gainAt()` takes
the clip's length rather than the fade storing one: a clip's length changes
under a trim, and a fade-out that did not follow it would slide into the middle
of the clip on the first resize.

It reaches the mix through `STrack::refreshClipGainCurves()` — the one
main-thread funnel a clip's gain already travels, and NOT through the cut,
which may not know its track — and `twTrackMix` applies it as a THIRD FACTOR in
the per-frame product it already computes for the gain curve and the static
volume. `ClipEntry::fade` is APPENDED to that struct, never inserted: the entry
is aggregate-initialised in `addClip`, so a field in the middle would silently
shift every positional member past it.

A fade-in and a fade-out that OVERLAP (a clip shorter than their sum) both
apply, as a product. That is the honest reading of "both are declared", it
degrades continuously as the clip is trimmed, and it makes a very short clip
fade to a peak below unity rather than jump.

**The SHAPE is shared with the crossfade N3 will build**, deliberately: a
crossfade is two fades that meet, and two curve families that could disagree
about the shape of the same join is one more than there should be.
`EqualPower` is `sin(t·π/2)`, whose square sums to 1 across a join.

Gate: `clip_fade.qxa`, all closed forms over `test_gapsaw.wav`. The
equal-power number (0.06262 against linear's 0.05401 over the same second)
is what makes the SHAPE gateable rather than just the length.

### `place-recording` may carry a NEGATIVE source offset

`SPlaceRecordingAction`'s `srcOffset` is the point in the recorded WAV the
placement starts at, and it is **signed**: a negative value is LEADING SILENCE
(proposal 23 -- "the clip then opens with silence and its data starts later"),
which is how a placement expresses material that begins partway in.

It used to be clamped to 0 on the way in. Nothing needed the negative half
until loop recording had to place a pass that STARTED MID-CYCLE at the loop
start and one loop long -- a take whose material begins partway in is exactly
what such a pass IS, and no non-negative offset can represent it. The clamp is
therefore what forced the recorder to chop the performance at every wrap
instead (`main/shell/CONTRACT.md` inv. 27).

The machinery already handled a pass that ENDS early, because running past the
end of the sample is silence too; the asymmetry was that one line. `span` still
derives from `waveDur - srcOff`, which grows by the leading silence, and is
still capped by an explicit `length`.

Gate: `record_loop_late_start.qxa`.

### `place-recording` is a THIN VERB; one `srecplan::Planner` per record mode

`srecordplanner.{h,cpp}` (plan 53 §5.1, QBX-171). The verb resolves the lane,
reads the file's duration, computes the pass's span — and then decides nothing.
It builds an `srecplan::LaneSnapshot`, asks `plannerFor( mode )`, and applies
whatever the planner appended to one `SCompositeAction`.

Three rules carry it, and every planner is held to them:

1. **A PLANNER EMITS EXISTING VERBS AND NEVER MUTATES THE MODEL.** Its undo is
   therefore the composite's inverse chain — there is no per-mode undo to get
   wrong — and a planner is testable by the verbs it emits rather than only
   through the model it leaves behind.

2. **`snapshot()` IS THE ONE READER OF LANE STATE, AND IT CLASSIFIES WITHOUT
   MUTATING.** No planner may look at the lane again: halfway through its own
   plan it would be reading a tree its own earlier verbs have not been applied
   to. A child whose object `isLiveRecording()` is skipped outright (a growing
   clip of this or any other recorder). A wrapped column is reported as
   `WrappedColumn` and **`snapshot()` does NOT call
   `stakes::normalizeColumns`**: that takes the whole `SProject&`, relinks and
   deletes wrappers outside any action, and is a post-load pass — running it
   from inside an `apply()` would put a non-undoable mutation in the middle of
   an undo step. After the post-load pass a surviving wrapper is non-foldable
   anyway, so classifying it loses nothing.

   `OtherKind` is decided by the PLAN, not by `snapshot()`, because it is not a
   property of the child: an event clip is other-kind to an audio recording and
   is the whole point of a MIDI one. So `Child` records what the child IS
   (`contentKind`) and the plan compares it against `Context::kind`.

3. **INDEX BOOKKEEPING IS THE PLANNER'S, IN APPLY ORDER** (plan 53 G10). Lane
   child indices are INSERTION order, not time order, and the emitted verbs
   change them: `place-clip` and `split-clip` APPEND, `add-take` preserves its
   operand's index (it replaces the lane child link in place),
   `merge-columns` keeps its FIRST operand's index and REMOVES the others, so
   every later index shifts down. `srecplan::ChildList` is the planner's
   simulation of the list and the only place a path is computed from once
   anything structural has been emitted. The old planner's comment "paths are
   computed NOW and stay valid through the plan" was true only because it
   emitted nothing but appends; it is false from the first split or merge, and
   a plan that got it wrong would address a WRONG child SUCCESSFULLY — silent
   corruption rather than a failed action.

**`mode` arrives already resolved** and the verb cannot resolve one:
`tools/check_layering.py` gives `objects/cut` the edges
`{actions, model, objects/wave, persistence}` and not `objects/track`, where
`srecmode::resolveMode`'s folder walk lives. `SAudioRecorder` resolves per
armed lane and submits the answer (`main/objects/track/srecordmode.h` says so
at length). Empty, and any value this build does not know, mean "whatever this
build would do anyway" — which is `LegacyTakes`.

**NOTHING HAS FLIPPED YET.** `Mode::Takes` is `LegacyTakes`, today's planner
moved with its behaviour intact, and it is the resolved default everywhere, so
no recording made by the recorder lands differently than it did before plan 53.
`Overdub` is one `place-clip` over whatever the lane holds. `TakesSplit` and
`Replace` are implemented (QBX-172) and are reachable only by asking for them.

**`TakesSplit` (mode `takes-split`, plan 53 §3.2).** Every touched child is cut
at `a` and at `b` where those fall inside it; the piece inside `[a, b)` becomes
a column with the old piece as take 0 and the pass as take 1, active; the pieces
outside are untouched; the sub-spans of `[a, b)` no child covers become plain
cuts. A take is therefore an alternative for exactly the span it covers, which
is why this mode needs none of §4's take-extent model change and could land
before it. Two things happen by themselves and are deliberately not
re-implemented: `add-take activate=1` CLEARS the inside piece's comp map
(QBX-159), and a split column CARRIES its map to both halves (QBX-160).

**`Replace` (mode `replace`, plan 53 §3.3).** Every touched child LOSES
`[a, b)`, then the pass is laid down as one plain cut. Never wraps, never
stacks. All four cells reduce to ONE code path shared with `TakesSplit`:
isolate the inside piece by cutting, then REMOVE that piece instead of adding
a take to it — R1 cut at `a` and remove the tail, R2 cut at `a` and `b` and
remove the middle, R3/R5 remove the child outright, R4/R6 cut at `b` and
remove the head.

It is expressed as `split-clip` + `remove-sample` and NOT as the left-edge
`resize-clip` plan 53 §3.3 and §12 name. A left trim has to name the piece's
new SOURCE anchor, and the exact one is
`SClipWindow::timelineToSourceExact( b - s )`, which runs through the clip's
warp map — so `srcStart + (b - s) * stretch` is wrong for any warp-anchored
clip, and rule 2 forbids a planner from reading the lane again to ask.
`split-clip` computes that anchor exactly, inside itself, and
`comp_map_survives_split` already gates it, so cutting at `b` and removing the
head yields the identical window with no new arithmetic anywhere.

**A CUT MUST CLEAR BOTH EDGES BY TWO FRAMES, not merely fall inside.**
`SSplitClipAction::apply` refuses `inObjOffset <= 1` and
`inObjOffset >= fullDur - 1`, and `SCompositeAction::apply` rolls the WHOLE
composite back on the first child that fails — so a planner emitting a cut one
frame inside a clip loses the entire take, which is the one thing a recorder
may not do. Plan 53 §3.2's "where those fall strictly inside" is NOT that
predicate. `pieceCanSplitAt()` is the guard, and a cut that close degrades to
the no-split cell, whose take anchor then carries the ≤ 2-frame difference as
leading silence (a negative `srcOffset`, legal since proposal 23). §3.5's
pass-wide snap removes almost every occurrence before a planner sees it, but
only when every armed lane agrees.

**BOTH NEW PLANNERS REFUSE A LANE HOLDING A WRAPPED COLUMN** (G11, the matrix's
"any R · Kw" row). That is a POLICY now and no longer a forced hand, which is
worth saying because the plan's stated reason has expired: §3.3 and §12 said
`replace` must refuse a Kw lane because `remove-sample` could restore only the
direct shape, and QBX-155 landed and it now pins BOTH. What keeps the refusal
is the rule itself — a surviving wrapper is non-foldable, both modes would have
to split or delete it, and no gate has ever exercised one. The refusal is
announced, costs the pass only that lane, and is what
`record_lane_skipped_reported` (QBX-164) is waiting for.

**THE PLAN IS HELD BY THE ACTION, never built on the stack.** A child's inverse
may BORROW the child: `remove-sample` pins a take column and hands back an
`SRestoreTakeColumnAction` whose only link back is `owner_`, documented in that
header as "holds the pin; borrowed". A stack composite is destroyed when
`apply()` returns, which deletes the child, drops the pin, and leaves the
inverse on the undo stack pointing at freed memory — measured as a SIGBUS in
`rec_replace_cells` before `SPlaceRecordingAction::plan_` existed. Holding it
makes the lifetimes right by construction, because `SActionUndoCommand` owns
the forward action and the inverse together and destroys them together.
`plan_` is not a second source of truth: nothing reads it back, and it is
rebuilt from scratch on every apply, a redo re-planning against the lane as
the undo left it.

**Overlapping children go LEFT TO RIGHT under `takes-split`, which deviates
from plan 53 §3.4 and is recorded there.** §3.4's mode-B bullet says the
TOPMOST inside piece receives the pass. That is well defined only when one
piece contains another; for a PARTIAL overlap it names no answer for the part
the topmost piece does not cover, which would lose recorded audio. The cursor
rule gives the pass to the FIRST piece covering each position and places it
exactly once — agreeing with §3.4 wherever §3.4 is precise (W1's adjacent
children) and never losing audio where it is not. "Topmost" depends on decision
D1, which P2 owns.

**One deliberate deviation from plan 53 §5.1, recorded there too:** a wrapped
column is handed to `LegacyTakes` as an ordinary column. §5.1 says every
planner refuses one (G11) and P1 says "Flips: none"; both cannot hold for this
planner, because today's planner hands a wrapped column to `add-take` like any
other. The refusal is the NEW planners' rule; `LegacyTakes` keeps the old
behaviour until P2 retires it.

**`ChildList`'s REMOVAL SHIFT IS GATED since QBX-172**, by
`rec_replace_cells`' fifth track: two children both inside the pass, so
`replace` removes the first and the second's index moves DOWN to the slot the
first vacated. The second `remove-sample` must name the shifted index, and a
planner computing its paths from the snapshot would name the original one and
delete whatever had moved into it — successfully, which is silent corruption
rather than a failed action. Verified by SABOTAGE: replacing
`live.indexOfOriginal( c.index )` with `c.index` fails the case.
`takes-split` can never reach that path, because it only ever APPENDS, which is
why the gate had to wait for a mode that removes.

**WHAT IS STILL NOT GATED, and why.** `ChildList` is built from `LaneSnapshot::laneChildCount`
— the lane's REAL child count — and not from `children.size()`, because
`snapshot()` leaves out a live recording and a child with no duration, so a
`Child`'s `index` may have gaps below it. A simulation built from the
classified subset would map every index above a gap one too low and address a
real child, just the wrong one.

No `.qxa` can reach that today, and a case claiming to would be theatre. The
two sources of a gap are a child with `hasDuration()` false, which nothing in
the model produces on a placement lane, and a child whose object
`isLiveRecording()` — which exists only DURING a take and which
`removeGrowingClips_()` removes before the plan runs
(`main/shell/CONTRACT.md` inv. 26). A sub-track is not a gap: it is reported as
`Child::Lane` and keeps its index. The guard is therefore correctness against
the next producer of either, not a fix for a reachable defect, and it is
recorded here rather than asserted.

**`snapshot()` takes the lane path as well as the lane**, where §5.1 wrote
`snapshot( SObject *lane )`. An `SObject` cannot name its own path: the tree is
`SObject -> SLink -> SObject` and only the links point down, which is the same
fact that makes `srecmode::resolveMode` and `ssolo::underSoloedAncestorRec`
walk downward from the root. The caller already holds the path — it is how it
found the lane.

Gates: `record_mode_overdub_audio` (the shape AND the summed level, in closed
form, on a fixture where `overdub` and the legacy planner cannot agree by
accident), plus the whole `place_recording_*` / `takes_recording_*` /
`record_*` families unchanged, which is what makes `LegacyTakes` a move rather
than a rewrite.
