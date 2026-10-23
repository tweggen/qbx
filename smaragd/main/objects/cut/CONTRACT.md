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

**`merge-columns` FOLDS A LANE INTO ONE UNION COLUMN, and is the only verb
here that REMOVES** (plan 53 §6.1, A1). It keeps its first operand's child
index and removes the others, so every later index shifts DOWN — the reason
`srecplan::ChildList` models an index SPACE rather than a list. Removals are
issued in DESCENDING index order, so none invalidates an index still to be
used.

Plain clips become takes at their own extents; a column's takes transfer at
their extents shifted by `(colStart − newStart)`, and its map with them. The
combined map is each operand's PREVIOUS ANSWER over its own extent, the later
operand winning an overlap, with the previous answer re-entering at each
operand's end — the same shape `select-take` uses for an extent (§4.3). A GAP
between operands becomes a span no take covers, which plays silence, which is
what the lane played there before; that is the whole reason this verb needs
A0a.

THE WINDOWS ARE MOVED, NOT COPIED, so every per-clip property travels for
free. Its inverse `unmerge-columns` is LIVE-ONLY and re-links the SAME
objects, finding each take by its window OBJECT rather than by an index —
every move shifts the indices above it. It restores child ORDER as well as
existence (QBX-149).

**THE FORWARD PINS EACH OPERAND COLUMN, which §6.1 says is unnecessary.** It
is not: an `STakeStack` is an `SObject`, so it carries its own `volume`, `pan`,
`muted`, `solo` and `recordingChannels` (`SObject::serializeSelfAttributes`),
none of which lives on a take or on a link. Rebuilding the column from a
recipe would silently drop them, which is the precise failure plan 52 exists
to prevent. The TAKES need no pin — they are children of the merged column
until the inverse moves them back.

**AND IT REFUSES ANYTHING IT CANNOT MERGE WITHOUT CHANGING WHAT PLAYS.** Over
and above §6.1's list (mixed kinds, a wrapped column, a live recording, a
lane), every operand must agree on `getVolume()` and `getPan()` and none may
carry a `cut:Gain` envelope or a clip fade. The mix reads a placement's
parameters through `SClipWindow::parametersOf`, which resolves a DIRECT column
to its ACTIVE TAKE — so a merged column has exactly ONE set of clip parameters
and the mix applies them across the WHOLE column. N operands at different
levels cannot all be honoured, and a curve or a fade is measured against the
clip's own length, so even identical ones would mean something else in a
longer clip. Plan 53's criterion for the row is that the render does not
change, so the verb refuses rather than re-mixing the lane silently. This is
QBX-185 in a second place (per-take gain is applied ABOVE the column); when
that is fixed the refusal can narrow to what genuinely cannot be carried.

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
  statement is the same one. **The PIXELS are gated since P7** (QBX-157,
  `take_comp_backdrop.qxa`): A0b gated that the span is silent and its envelope
  empty, and recorded at the site that reverting the paint itself to the old
  active-take draw passed the whole suite. P7 grabs the composite lane and
  asserts the comp result's SHAPE on it, next to the same three quarters
  measured as ENERGY on one fixture.
- The COMPACT painter also RULES THE CUTS (plan 53 §4.4 part 2, P7 / QBX-157):
  a mark at every boundary of the same run walk the paint makes — so a comp
  boundary and the edge of a take's EXTENT are marked by one rule, because to a
  listener they are one thing — and at the column's own two edges, which is the
  case the brief is about (two columns a `takes-split` pass left abutting carry
  the same body colour over continuous-looking material, and the cut between
  them is otherwise invisible). An edge is told apart from a VIEWPORT CLAMP by
  TIME, in one pixel's worth of it, because the clip loop hands this renderer a
  rect already cut down to the viewport — and that same test answers correctly
  for a slipped WRAPPER window, where the clip edge is a window edge and not a
  cut in the material. The colour is `sclipcolors::cutMark`, derived from the
  clip anchor like every other variant and therefore readable by the pixel gate
  through the same function the painter uses. **The mark is two short TICKS and
  not a full-height line**, and that is load-bearing: the pixel classifiers
  decide "material" per column by whether the body colour appears in it, so an
  opaque full-height mark would read as a GAP — a cut mark manufacturing the
  discontinuity it describes.
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
  two ends because it has to MIX them. **And P7's cut marks do not draw one
  either**: the span a band would have to cover is not `twCompSegment::xfade`
  but `twCompColumn::clampedXfade_nolock`, which folds in P5's project
  auto-fade as the fallback and clamps against both neighbouring boundaries and
  both takes' extents — private to `tw303a/mix`, and a painter recomputing it
  would be the parallel interpretation this whole section forbids. A boundary is
  a line, which understates a fade but never misstates it; exposing the clamped
  length is a public-header change and has its own ticket.

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

**`select-take take="newest"` (plan 53 §6.5, QBX-182) is the one non-numeric
take value**, and it is resolved PER COLUMN at apply time: among the takes whose
name carries the recorder's stamp (`stakenames::isRecorded`) the highest index,
else the highest index of all. Under an edit-group broadcast the RULE travels
and each member resolves it — which is the point, since the pass just recorded
is take 7 on one lane of a band take and take 2 on the next, and no single index
could say that. The ORDER is the index and not the stamp's value: `HH:MM` has no
date and two passes in one minute tie, while a recorded take is always APPENDED
(`merge-columns` names the pass as its final operand, `add-take` appends), so
the column's own take order IS recording order. The INVERSE is always a plain
index — undo restores a take, not a rule.

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

## `merge-columns` may be handed its EXTENT, and then ONE operand is enough (plan 53 P4, QBX-182)

`merge-columns start= length=` names the merged column's extent outright
instead of taking `[min start, max end)` over the operands. `length < 0` is
"the operands' union", which is what every call written before P4 means and why
none of them changed.

It has exactly one caller: **mode A planning an EDIT GROUP.** Correspondence
inside a group is POSITIONAL — `seditgroups::correspondingClip` matches
`startTime` AND `getDuration()`, exactly — so every member's column has to be
the same `[startTime, duration)` or the lanes of one band take stop being each
other's twins and every later grouped edit applies to one lane only. That
shared extent is the group extent `C_g`, the union of the ARMED members' own
extents, so on a member holding less material it is WIDER than that member's
operands.

Two consequences follow from that and are the reason the parameter is not just
a convenience:

- **ONE operand is accepted with an explicit extent and refused without one.**
  One is the common case here — a member holding the single clip the armed lane
  was played against — and the result is §4.2's UNION PLACEHOLDER, a one-take
  column whose take covers less than the column. Without an extent, one
  operand would produce a column identical to the clip it wraps, which is the
  shape invariant 3 says must not exist; with one, invariant 3′ makes it legal
  and `collapseSingleTakeStack` already refuses to collapse it
  (`takeIsFullExtent`).
- **The extent must CONTAIN every operand, and a narrower one is REFUSED.**
  Clipping to fit would silently drop a take's material, and
  `unmerge-columns`' recipe records each take's extent rather than a truncated
  copy of it, so the clip could not be undone either. A caller that wants the
  shorter column wants `resize-clip` on the result.

The existing "a gap between operands plays SILENCE" rule is what makes the
widening inaudible: the part of `C_g` no take covers played silence before the
merge too.

## An EVENT column follows the BAR; an audio column does not (plan 53 P3, QBX-181)

A take stack is window-typed, so one class serves both content kinds — and
three of its stored FRAME values have no musical anchor, which is correct for
audio and wrong for events the moment the tempo moves. An event take's window
derives its length from `lengthTicks_` and its link is born `timebase=beats`,
so `set-tempo` moves both ends of every extent behind the column's back.

| what | audio | event |
|---|---|---|
| `duration_` (the column's own length) | stays as set — A0a deliberately lets a column outlast its takes, and a span no take covers plays silence, which is what a gap between `merge-columns` operands relies on | follows `derivedDuration()` on any take-duration change (`onTakeCutChanged`) |
| the COMP MAP's segment positions | frames in a recording; must NOT move | tick-anchored in `compTicks_` and re-derived by `rederiveMusicalPositions()` |
| a comp `xfade` | an equal-power crossfade length | left alone: notes do not crossfade, so an event comp boundary is a hard switch |

Measured on a one-bar union column at `set-tempo bpm="60"` before the fix:
`extents=[0-192000;0-192000] duration=96000` — the takes reach 192000 and the
column says 96000, so invariant 1′ is false and the clip's tail is cut off. And
on a 32-bar one: the extents doubled while the map still named the OLD frames,
so the recording sounded over the bars before its own and over none of them.

`compTicks_` is `SLink::startTicks_` for a comp map, written whenever the map is
and read back when the tempo moves. It is NOT serialized and does not need to
be: a project file carries one tempo, so re-deriving the ticks from the loaded
frames gives the writer's own answer. `set-tempo`'s `rederiveBeatsLinks` calls
`SObject::rederiveMusicalPositions()` on every object in the same loop that
re-derives the links, and the base answers false.

## `resolveEventClip` on a column is EXTENT-SHIFTED, and resolves the ACTIVE take only

The track inserts a column as ONE clip and hands positions in the COLUMN's
frames; a take answers in its own, where zero is its extent start. Unshifted, a
recording over bars 8–16 of a 32-bar part SOUNDED AT BAR 0 — measured, the feed
gave the pass's notes at frames 0 and 24000 where they were played at 768000 and
792000. The shift composes the take's own map (its slip, its loop, and the
`runFrames` an enumeration needs) rather than replacing it.

What it still does not do is consult the COMP MAP, so everywhere the active take
has no material the column plays SILENCE instead of the take the map names.
That needs the event equivalent of `twCompColumn` and this seam cannot express
it — `twEventClipResolved` is one sequence per clip, and two takes are two
sequences. **QBX-189**, with the fix sketched (the column synthesises one
sequence from its takes through an internal `twEventClipSet`).

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

> **THE PLANNER NO LONGER LIVES HERE (QBX-188).** It moved to
> **`app/objects/recplan`**, whose `CONTRACT.md` is now the authority on the
> planners themselves — the rules they are held to, `PassVerbs`, and what
> `snapshot()` may ask. It had to move: it sat beside the verbs it emits, which
> put it out of the EVENT side's reach for good (`objects/midi` may not depend
> on `objects/cut`, proposal 37 §3.5), so plan 53 P3 could not be the wiring it
> was scoped as. The new module sits at the RANK of both and neither depends on
> the other.
>
> What stays THIS module's business is the half below plus
> **`SAudioPassVerbs`** — the audio implementation of the seam, and the one
> place the pass's SOURCE arithmetic lives (`srcOffset + (at − a)`). The
> sections after it that describe a planner's own behaviour are kept here for
> the measurements in them and point forward where they describe the move.

`place-recording` (plan 53 §5.1, QBX-171). The verb resolves the lane,
reads the file's duration, computes the pass's span — and then decides nothing.
It builds an `srecplan::LaneSnapshot`, asks `plannerFor( mode )`, supplies
`SAudioPassVerbs` as the `Context`'s verb set, and applies whatever the planner
appended to one `SCompositeAction`.

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
build would do anyway" — which since P2 is **mode A**.

**`Mode::Takes` IS MODE A (plan 53 §3.1, QBX-180 / P2), and this is the one
row of plan 53 that changes what recording DOES by default.** A pass over a
lane that already holds material no longer lands beside it: the touched
children and the pass become ONE union column over
`C = [ min(a, min sᵢ), max(b, max eᵢ) )`, every touched child a take at its
own extent, the pass the active take, and the comp map the pass inside
`[a, b)` with what was there before everywhere else. `LegacyTakes` is gone —
removed, not left unreachable. `Overdub` is still one `place-clip` over
whatever the lane holds; `TakesSplit` and `Replace` (QBX-172) are still
reachable only by asking for them.

**MODE A'S PLAN IS TWO VERBS**, which is why A1 (`merge-columns`, QBX-179) had
to land first:

```
place-clip     the pass, at `a`, over its EXTENT
merge-columns  every touched child IN ASCENDING CHILD INDEX, then the pass
```

`merge-columns` already is §3.1 steps 1, 2 and 4; naming the pass LAST gives
step 3 for free. Four things follow, each measured rather than assumed:

1. **NO GAP FILLING.** Every member of `T` intersects `[a, b)`, so the one
   child that can start before `a` must also end after it and therefore covers
   `[C.start, a)` entirely; the same holds at the other end, and the pass
   covers `[a, b)`. `C` is contiguous AND fully covered, so unlike the other
   two planners this one has no uncovered sub-span to place a cut over.
2. **ASCENDING CHILD INDEX IS D1's TOPMOST RULE.** Where two touched children
   overlap, the previous answer is the one with the HIGHER child index, and
   `merge-columns` gives an overlap to the LATER operand — so the operand list
   is in index order, not the time order `columnsFor` returns. D1 (a) also
   means the SUM those two children produced is GONE; `rec_union_overlap_legacy`
   asserts the halving, so the decision is visible in the suite.
3. **THE PASS IS PLACED OVER ITS EXTENT, NOT ITS MATERIAL** (§3.1 step 5).
   `Span::extent` carries the cycle region, because `b` cannot: the verb clamps
   the span to the material, so the last pass of nearly every cycle recording
   is short, and its take must still be an alternative for the same bars.
4. **A WRAPPED COLUMN REFUSES THE LANE** (G11), as under the other two new
   modes — and since `takes` is the default, that refusal is now reachable
   without asking for it. It is a FLIP: the planner before mode A handed a
   wrapper to `add-take` like any other column.

**MODE A PRE-FLIGHTS `merge-columns`' PARAMETER REFUSAL AND DEGRADES.** The
verb refuses operands that disagree on volume or pan, or that carry anything
POSITIONAL — a `cut:Gain` envelope or a clip fade — because a merged column has
ONE set of clip parameters (QBX-185). That is right for a user gesture and
fatal for a recording: `SCompositeAction::apply` rolls back every earlier child
on the first failure, so the pass's own `place-clip` would go with it and the
take would be LOST. The planner therefore asks the same questions of
`LaneSnapshot` (`Child::mixVolume`, `mixPan`, `hasGainCurve`, `hasFade`,
`hasWindow`, `nTakes`) and, when any of them says no, places the pass as a
plain clip over the lane and ANNOUNCES it. That degrade is chosen because it
changes no existing child: a LegacyTakes-shaped one would wrap the user's faded
clip into a column, where `parametersOf` resolves to the ACTIVE take and the
clip's own fade stops applying — the degrade would itself change what plays,
which is what the refusal exists to prevent.

**`merge-columns` NO LONGER WRITES A DEGENERATE MAP**, and mode A is why. One
segment at 0 naming the take that becomes active, where that take covers the
whole column, answers exactly what no map plus `activeTake_` answers — but it
SERIALIZES. As a user gesture that case is rare; as the recording planner it is
§3.3's R3 and R5 cells, which is every cycle pass after the first and every
pass over a clip lying wholly inside it, so writing it would put a `<comp>`
element into essentially every project that records twice. §3.3 writes those
cells' map as "`0→R` (= empty, active R)"; this is what makes that true.

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

**`remove-sample` DELETES ANY CLIP BUT A LIVE RECORDING, and the pin is the
DEFAULT inverse** (QBX-146 step a, widened by QBX-202). The two
rebuild-from-parameters branches keep their priority where they apply —
file-backed content hands back `add-sample` with the whole window,
container-backed hands back `restore-container-clip` — and every other content
is PINNED with `addRef()` and re-linked by identity. Step (a) reached only a
take column, which left the commonest un-rebuildable shape in the app refused:
a MIDI clip, whose content is an `SMidiSequence` and so is neither an
`SExternFile`, nor a path container, nor a column. Deleting a recorded MIDI
clip therefore did nothing but write the refusal, which is how the author met
it. There was never a fidelity argument for the narrow reach — the pin rebuilds
nothing and needs to know nothing about what it holds — so the conservative
thing is the ORDER, not the reach.

What stays refused is a LIVE recording, and that is OWNERSHIP, not fidelity:
`SAudioRecorder::Armed` holds the growing `SCut` and its `SLink` as borrowed
raw pointers and deletes them at stop, so the delete is a use-after-free in the
recorder before undo is even reached. `isLiveRecording()` is the predicate,
which `SCut` forwards to its content, so this module never names
`SRecordingContent`'s type. The MIDI recorder has no such exposure by
construction: it buffers its messages and submits `place-midi-recording` once,
at the stop, so no growing MIDI clip is ever in the model to delete.

**THE PIN CARRIES THE CONTENT BY IDENTITY, AND THE LINK'S OWN FIELDS BY HAND.**
`SLink` holds three things that live on the placement rather than the object —
`timebase_`, the exact `startTicks_` and `eventChannelOverride_` — and the
inverse builds a NEW link, so without carrying them a clip switched to the
TIME timebase comes back on beats (`SLink::defaultTimebaseFor` makes Beats the
default for event content) and a placement channel remap comes back as
as-authored. They are captured on the forward's apply and applied BEFORE
`setParent`, because `STrack::trackChildWasAdded` resolves the new child's
event feed through the link it is handed. Same three fields, same order and
the same reason as `merge-columns`' `carryLaneLinkAttrs()`; the loss was
already there for a take column before QBX-202 widened the reach.

**THE PLAN IS HELD BY THE ACTION, never built on the stack.** A child's inverse
may BORROW the child: `remove-sample` pins a take column and hands back an
`SRestorePinnedClipAction` whose only link back is `owner_`, documented in that
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
children) and never losing audio where it is not. "Topmost" was decision D1,
and P2 took option (a): mode A merges such a lane and gives an overlap to the
HIGHEST child index. Mode B keeps the cursor rule, which is its own answer to a
question mode A does not have — B never makes one column out of two children.

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
`record_*` families, plus mode A's own `rec_union_r1`…`rec_union_r6` (one per
R value, Kp/Kc/Km on three tracks), `rec_union_w1` (the case plan 53 was
written for, with the undo gated by render identity),
`rec_union_overlap_legacy` (D1 (a) made visible), `rec_union_cycle_extent` (the
whole-cycle extent, exactly) and `rec_union_cycle_inside` (the same cell live).

**THREE EXISTING GATES MOVED WITH MODE A**, each edited with its reason in its
own header: `takes_recording_placement` (the silence it pinned over
`[192000, 288000)` WAS X2 and is now the old material), `place_recording_over_earlier_clip`
(its claim is unchanged — a take must never be discarded — but a surviving take
is now observed as a take with an extent rather than as a new lane child) and
`record_snap_disagree`, which plan 53 §9 did not predict: one of its two lanes
is R0 and the other R1, so only the second folds. `record_stays_armed`, which
§9 DID predict, does not move: QBX-171 moved its second pass 10 s away, so both
passes are R0.

## A clip fade carries a VETO as well as two lengths (plan 53 §8 P5, §6.2, QBX-183)

`twClipFade` gained `autoIn` and `autoOut`, both default true. They are not
lengths and are not read by the mix: they say whether the PROJECT's auto-fade
(`SProjectProps::AutoFadeMs`) may apply at that edge. Since the project has an
auto-fade, a length of 0 no longer means "no fade" — it means "whatever the
project says here" — so the state the user needs and could not express is a
HARD edge, and that is what a false flag is.

**PER EDGE, AND THE PLAN SAYS PER CLIP.** Plan 53 §P5 words the rule as "every
clip edge whose `twClipFade` is `none()`", which read literally says that
declaring a fade-IN turns the auto-fade off at the clip's OUT edge as well. It
is applied per edge instead: a user who drew one fade did not ask for a click
at the other end, and the exemption D3b makes is already per edge (a clip split
in two has one seamless join and one ordinary end, and "not at the join, yes at
the outer end" has to be sayable). Per edge is a superset of the plan's
reading, and `auto_fade_override.qxa` pins both halves of it.

Serialized as `autoFadeIn` / `autoFadeOut` on the `<cut>` element and ONLY when
false, the rule `fadeIn`/`fadeOut` and `<automation>` already follow: a project
in which nobody asked for a hard edge gains no attributes and re-serializes
byte-unchanged. Absent means TRUE on read, which is what lets the feature reach
every clip of a new project without touching one of them.

**EVERY SUBMITTER OF `set-clip-fade` MUST CARRY THE FLAGS THROUGH.** The action
is absolute over the WHOLE fade, so an edit that passes the ctor defaults
CLEARS a veto the user set at the other edge — drag the fade-in handle and the
hard fade-out silently becomes an auto-fade. The arranger's fade drag
(`SStdMixerView`) and the Clip Properties panel both read the current fade and
pass its flags back; the panel goes further and splits the commit in two
(`commitFade` for the lengths and the shape, `onAutoFadeClicked` for the
flags), each reading its own half from the widgets and the other half from the
model.

## `merge-columns`, `select-take` and the comp column's AUTO CROSSFADE (plan 53 §8 P5)

`twCompColumn::setAutoXfade( frames )` is the length a boundary with
`xfade == 0` gets. `STakeStack::ensureCompColumn` sets it from
`sautofade::framesOf( project )` on every ask, beside the takes, the extents
and the map — so a change to the project property is picked up by the next
resolve and there is no stale copy of the length anywhere.

It is applied in `clampedXfade_nolock`, which is the ONE function both
`planPage` and `freezePage` ask what a boundary's crossfade is. That placement
is the design: two frames of disagreement between the plan and the render is
proposal 19 Inv-1's failure, and a fallback written anywhere else could produce
it. Every clamp therefore applies to the auto-fade unchanged — half the
distance to each neighbouring boundary, and both takes' extents (§4.1).

**A COLUMN WITH NO BOUNDARIES CANNOT FADE, AND THAT IS WHAT KEEPS OLD PROJECTS
BYTE-IDENTICAL.** With no map and every take full, `getRootComponent` hands out
the active take's own component and no `twCompColumn` is built at all (the
degenerate case, proposal 43 N2). The auto-fade reaches exactly the joins that
exist. Nothing about the exemption D3b makes applies at a boundary either: two
takes are two performances, so the waveform steps there however tight the comp.

## `cleanup-takes`, and why a REMOVAL has to be undoable first (plan 53 §8 P6, §6.3, D5 (a), QBX-184)

Comping leaves rubbish: every pass that was not used is still a take on its
column. `cleanup-takes` sweeps the ones nothing selects — not the active take,
not a take any comp segment selects over a position where it has material, and
not the user's own pre-existing clip.

**"CAME FROM A RECORDING" IS A NAME TEST**, and that is decision D5 (a) rather
than an implementation shortcut: `stakenames::isRecorded` asks whether the
take's name carries the recorder's stamp. A take that was the user's clip —
W1's original A, which recording turns into an inactive take without them
doing anything — is EXEMPT unless `includeOriginals` says otherwise. The sweep
reports both numbers so the confirmation can say "3 recordings, 1 original clip
(kept)".

**THE PLAN IS COMPUTED BEFORE ANYTHING MOVES, AND APPLIED HIGHEST INDEX
FIRST.** `removeTake` shifts every take above the one it drops, and renumbers
the comp map with it, so deciding and removing in one pass would read indices
the previous removal had already changed.

**AND THE PLAN IS ITS OWN FUNCTION** (`scleanuptakes::plan`, QBX-195), because
the CONFIRMATION has to state what the sweep will do before it does it. The
dialog and `apply()` ask the same walk, so the numbers a user agreed to and the
removals that follow cannot disagree; a dialog with its own count would be a
second reading of D5 (a)'s name test. The dialog cannot ask it directly —
`app/servicesui` may not depend on this slice — so the ARRANGER asks and hands
the numbers over, which is also why `Plan` carries `removedNames`: the one
consumer that needs them is a label. The plan lists doomed indices ASCENDING
(what a list wants); `apply()` reverses them, which is the one place the order
matters.

**THE ACTIVE TAKE IS NEVER SWEPT, AND `includeOriginals` DOES NOT OVERRIDE
THAT.** The `t == active` test runs before the name test, so a column whose
original clip is also the take that sounds loses nothing however the box is
set. Worth stating because the obvious reading of the checkbox is the other
one.

**WHICH MADE `remove-take`'s NULL INVERSE A BLOCKER, not a wart.** A sweep over
an event take could not be undone at all, and the one thing a destructive sweep
must be is undoable — so the pin pattern (QBX-184) landed first, as its own
change. The macro's children are `remove-take`s, and ONE non-undoable child
makes the whole composite non-undoable (`SCompositeAction`'s own contract), so
a single MIDI take in scope would have cost the entire sweep its undo.

**THE MACRO LIVES ON THE FORWARD ACTION.** `SActionUndoCommand` keeps the
forward action for redo, and each `SRemoveTakeAction` child holds the pin its
inverse reads back through a raw pointer. Deleting the macro after applying it
would drop every pin and leave each `restore-take` with a dangling owner: the
undo would refuse, and the sweep would silently stop being undoable for exactly
the takes the pin was added for.

**A TAKE'S NAME IS DATA, NOT DECORATION.** It is the origin marker, so
`add-take` had to start carrying one: without it the `remove-take` inverse gave
back an unnamed take, and undoing a removal turned a recording into an
"original clip" — permanently exempt from clean-up and invisible to
`select-take take="newest"`. Measured, not predicted: a restored take read
`(untitled)` and a second sweep kept what the first had removed.

### Known debt

`scope="selection"` is not available, and §6.3 specifies it. This slice cannot
see a selection (`objects/cut` has no edge to `shell` or `selection`), so the
verb takes `clips=` and the caller translates — the split `split-selection`
already makes against `split-clip`. **The caller exists since QBX-195**
(`SStdMixerView::cleanupTakesGesture`, behind the Edit menu, the lane context
menu and the Unused recordings dock's button), so this is no longer "nothing
but a script reaches the verb"; what remains is that the verb's own `scope=`
vocabulary still does not match §6.3's, and will not, for the layering reason
above.

`files="1"` is still exercised by no case, which is deliberate (moving files
about inside a `-j4` suite is not something a test may do) and still means the
trash move itself is hand-verified. The COUNT is gated, and since QBX-195 it is
also what the confirmation shows, through the same `srecfiles::scanUnused` the
dock reads.

## `select-take whole="1"` — hear one take alone (QBX-186)

Plan 53 §4.3 made the take-lane click EXTENT-AWARE: it selects the take
wherever it has material and keeps the previous answer elsewhere. That is
right — clearing the map on a PARTIAL take makes every span the take does not
cover play SILENCE, so a click would delete the other takes' audio without
touching one of them — and it took away the only way to say "this take over the
whole column", which is the gesture a comping user reaches for first.

`whole="1"` is that, and it is a FLAG on the same verb rather than a second
one, because the operation IS §4.3's full-extent branch: the only difference is
which branch runs.

**It clears the map.** Not as an optimisation: a one-segment map `{0 -> take}`
answers exactly what an empty map plus `activeTake_` answers, but it
SERIALIZES, so a build that wrote one would start putting a `<comp>` element
into project files that never had one. The clear is the canonical spelling of
the same map, and only the saved bytes can tell the two apart — which is why
`take_isolate` asserts on the file and not only on the model.

**On a take that already covers the column it is a no-op** equal to the plain
click. Both paths reach the same clear, so both renders are byte-identical;
`take_isolate`'s phase C pins that, as a regression guard rather than a gate
(it passes in a build that ignores the flag entirely). The flag must not invent
a one-segment map on the one path that has always cleared.

### The inverse does not broadcast, and that is a fix

An inverse is a plain restoring `select-take`, marked by
`setPreviousCompMap()`. It is now also built **non-broadcasting**, and that
closed a data loss rather than tidying a style: a broadcast forward pass fans
out into one child per member, each child returning its own inverse with its
own previous map, so the composite inverse already covers the group. An inverse
that broadcast as well re-expanded and rebuilt its children *through the
constructor*, which cannot carry `prevMap_`/`restoreMap_` — so every
re-broadcast child ran the FORWARD extent-aware computation instead of
restoring. Measured on a two-member edit group with `96000:1` on both columns:
one undo left `comp=[]` on both, the map gone with no way back.

Pre-existing since proposal 43 N1, and invisible because no case had ever
undone a BROADCAST `select-take` — `takes_group_broadcast` gates the forward
only. This is the standing lesson about an inverse that re-enters the same
fan-out it came from: the fan-out is the place the restoring state cannot
survive.

### Known debt

`twCompMap::normalized()` zeroes `front().xfade` on purpose ("no boundary to
fade across"), so a map whose ONLY segment carries a crossfade loses it before
anything can read it back — measured, a lone `at=96000 xfade=480` reads back
as `96000:1`. Whether a first segment at a non-zero position really has no
boundary (`activeTake_` sounds before it) is open, and belongs with QBX-196's
crossfade work rather than here. Cases that need a stored crossfade must state
a map with a segment in front of it.
## Each take's own gain reaches the mix through the column (QBX-185)

`ensureCompColumn()` pushes each take's volume, pan, fade and `cut:Gain`
envelope into `twCompColumn`, beside the extents and the auto-fade and on every
ask for the same reason those are: a `set-clip-volume take=` edit is picked up
by the next resolve rather than needing the column torn down.

It reads **the take's own window** — `takeAt( i )`, not
`SClipWindow::parametersOf` and not the active take. One take, one set of
parameters, which is the whole point: before this, these values reached the mix
only through the per-clip funnel, which composes one entry per placement from
the ACTIVE take, so an inactive take's own volume, pan and fade were carried,
serialized, editable and never heard.

`STakeStack::appliesOwnWindowGain()` answers `resolvesThroughComp()` — and
necessarily the same predicate, because that is exactly when a `twCompColumn`
exists and the column is the thing that applies them. A degenerate column
answers false and its active take's trim keeps reaching the mix through the clip
entry, which is what keeps it rendering byte for byte.

**An event take gets no fade**, the same guard and the same reason as the
auto-fade's in `refreshClipGainCurves`: a fade is audio-only (inv. 4) and an
event take's edges are note-offs rather than steps in a waveform. Its volume,
pan and envelope still travel.
