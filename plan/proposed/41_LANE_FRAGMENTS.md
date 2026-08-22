# Proposal 41 — Lane fragments: reusable same-lane clip groups, and the clip visual model

> **Status: PROPOSED (2026-08-21).** Two parts, deliberately in one proposal:
> the model change makes *disjoint* clip groups placeable, and the moment they
> are placeable the arranger's current painting is wrong. Part B exists because
> of Part A and is not independently motivated.

Prerequisite reading: `smaragd/main/objects/mixer/CONTRACT.md` (inv. 1-2, the
asset rules), `smaragd/main/objects/track/CONTRACT.md` (inv. 5b/5c, 9-10, the
child-sum walk), `smaragd/main/timeline/CONTRACT.md` (inv. 1, 10, 22),
`docs/contracts/POSITION_DOMAINS.md` rule 7,
`plan/proposed/17_TAKE_LANES_AND_COMPING.md` (what a take lane is, and is not),
`plan/proposed/37_MIDI_INSTRUMENTS_AUTOMATION.md` §3.2.1 (the event feed),
`plan/proposed/39_FOLDER_SUM_PREVIEW.md` (why a paint bug needs a pixel gate).

---

## Why

**1. There is no way to reuse a group of clips.** An asset today windows a
*container* — the root mixer or a folder track (`screateassetaction.cpp:47-61`,
"only containers can be windowed"). So the smallest reusable unit is a whole
lane with its own fader, inserts and instrument. A two-bar drum figure made of
three clips on one lane cannot be named, put in the bin, and dropped at bar 33.

The workaround is a container asset, and it is the wrong shape twice over: it
delivers **audio only** (an event clip inside it reaches no instrument, because
`STrack::eventFeed()` bubbles from child *tracks*, `strack.cpp:298-321`), and it
carries **track identity** — the fader, the inserts, the instrument of the lane
it was made from — so it cannot inherit the identity of wherever it is placed.

**2. Cubase's answer to this was ghost copies, and they did not survive
alignment.** A frame-anchored part pretending to be musical drifts against a
tempo change. This tree already has the machinery that fixes that
(`SLink::timebase`, `set-tempo` re-deriving `startTime` for every
`timebase=beats` link including inside assets, tick-native events at PPQ 960)
and no object shaped to benefit from it.

**3. The `SLink` model already promises what is being asked for.** There is no
original and no copy; placements of one registered asset are the same object
(mixer inv. 2 — "removing the last placement does not delete the asset"). The
asset list is a bin. What is missing is only *what may go in it*.

---

## The two rules this proposal adopts

> **A. A fragment has no track identity. It is an ordered set of clip links and
> nothing else — no fader, no inserts, no instrument, no solo, no arm, no
> meters, no automation of its own.**

Both behaviours fall out of that one property, and they fall out for the same
reason:

- **Audio clips in a fragment** sum with per-clip settings applied and no track
  FX, because there are no track FX to apply.
- **Event clips in a fragment** are consumed by nothing, so the fragment's
  *entire* event feed is residual and bubbles into whatever track the placement
  sits on. A BD fragment placed on any lane sounds through *that* lane's
  instrument, on that lane's channel.

> **B. A clip paints its MATERIAL, not its window. Of two clips whose painted
> material overlaps, the one with the LATER start time is on top.**

No transparency anywhere. Opaque, last-start-on-top, and the body is clipped to
where material actually exists.

---

## Part A — the model

### D1. A fragment is a container with no track identity

New object `SLaneFragment`: a path container holding clip links, a summing
`twTrackMix` at unity as its root component, and a residual event feed. It is
what `SCut` windows. Nothing else about it exists.

`SCut` already windows anything answering `isPathContainer()`
(`screateassetaction.cpp:57`), so the cut, the asset bin, multi-placement, and
`place-asset`'s `sarrangements::reaches` cycle guard all come free. **This
proposal extends what an asset may window; it does not add a parallel concept.**

### D2. The shared object is the CUT, not the fragment

Placements are pure `SLink`s to **one** registered `SCut`, differing only in
`startTime` (and `timebase`). One window, one capture, one preview, one aspect
set, and instance identity by construction — "all instances are the same" is
then true rather than approximately true.

The alternative (share the fragment, give each placement its own cut) yields N
windows that can quietly drift apart, N captures, and no well-defined notion of
"the same". Rejected.

**Sharing is the INVARIANT and is never broken.** One asset, N placements, edit
any and all change — the Unix hard-link model, and the entire point of the
feature. Nothing in this proposal un-shares an asset.

**A variation is therefore a NEW ASSET, not an un-shared placement.** The
timeline gesture ("Duplicate asset here") is a convenience that MINTS a new
asset — deep-copying the fragment, since D2 owns the fragment through its cut —
and repoints THAT ONE placement to it. The original asset and every other
placement are untouched, because they were never the thing being edited.

Read the other way round it is a defect: "make this placement unique" implies
an asset whose instances can diverge, which is exactly what an asset is not.
The implementation is the same deep copy either way; the mental model is not,
and it is the mental model that decides what the next verb does.

### D3. `isPathContainer()` currently means two things and must be split

It is documented as "descend into me for path resolution" (`sobject.h:221`,
`sobjectpath.h:118`) and is *read* as "I am a lane, and I carry solo / mute /
edit-group / arm" by `ssolorules.h:31,44,59,73`, `seditgroups.h:32,44,56,70,96`,
`splayheadmap.h:91`, `splacements.h:17,38,75` and `sinstrumenttracks.h:27`.

Only `SStdMixer` (`sstdmixer.h:72`) and `STrack` override it, so the two
meanings agree **by accident**. A fragment wants the first and emphatically not
the second: a fragment has no solo state, and `ssolo::anySoloInTree` descending
into fragments would let a fragment-internal flag darken lanes across the
project.

Split into `isPathContainer()` (descend) and `isLane()` (carries lane state).
`STrack` and `SStdMixer` answer both; `SLaneFragment` answers only the first.

**This is the load-bearing refactor and it is far cheaper before there is a
second container kind than after.** It is M0 for that reason.

### D3a. A fragment's CHILDREN are unaddressable by every clip verb — and that
is the price of D3, discovered in M2

`splacements::placementAt()` resolves through `laneAt()`, which requires
`isLane()`. A fragment answers **false** by design, so once clips are packed
into a fragment **no existing clip verb can reach them** — `resize-clip`,
`set-pitch`, `set-clip-pan`, `set-clip-name`, `select-take`, the formant verbs,
`sautomationactions` and the MIDI clip actions all resolve through that one
function and all refuse.

**This was not foreseen when this proposal was written, and AC2.3 as originally
worded assumed the opposite.** It is the exact trade D3 makes: the predicate
that stops a fragment's internal flags darkening lanes across the project is
the same predicate every clip verb uses to decide what it may address.

**A CORRECTION recorded by M2b:** the natural guess about WHY this would be
hard — that a fragment hangs off the asset list outside the root walk, so an
edit inside one would invalidate nothing — is FALSE. An `SCut`'s content link
is an ordinary parented `SLink` (`scut.cpp:1346-1347`), so the generic
recursive walk descends into a fragment exactly as into any other child, and
invalidation reached every placement with no fix at all. What WAS broken sat
one layer further on and only showed in rendered audio: `set-clip-volume` on a
fragment-nested clip invalidated correctly and then rendered BYTE-IDENTICAL
output, because `STrack::refreshClipGainCurves()` — which pulls a child's gain
into its owning `twTrackMix` — had no `SLaneFragment` equivalent. A model-state
assertion passes there. Only rendered output catches it.

It is a GAP, not a defect in D3 — the answer is the *edit-inside* affordance the
asset route was chosen for in the first place (open the asset, so the fragment
becomes the addressing root and its children are reachable relative to it,
exactly as an arrangement tab already does for a nested root). But it is real
work, it is not in the milestone list below, and until it exists a packed
fragment is **write-once**: placeable, shareable and unpackable, but not
editable in place.

What M2 DID gate is sharing at the CUT's own window — "edit any placement and
all change", D2's actual invariant — which is a weaker statement than AC2.3's
original wording and is recorded as such in M2's ACs.

**Deletion cascades, and that is INTENDED (decided 2026-08-21).** M2b's audit
found that widening `placementAt()` also lets `unplace-clip` and
`remove-midi-clip` reach a clip inside a fragment. Those DELETE a child link
rather than mutate a property, so they were flagged rather than silently
adopted. The decision is to keep them: a deletion is a shared edit like any
other, it cascades to every placement, and that is D2's invariant working
rather than an exception to it. Removing a clip from an asset removes it
everywhere the asset is placed — the same thing "edit any and all change"
means for every other edit.

Still true, and worth knowing: **`unpack-clips` remains the only way to move
material OUT of a fragment.** `move-clip`/`place-clip` resolve their
DESTINATION through `laneAt()`, which stays strict, so a clip can be deleted
from a fragment but never relocated into or out of one. NOT GATED: no case
exercises the deletion path inside a fragment.

**M2b (NEW, unscheduled): addressing inside a fragment.** Extend clip-verb
addressing to descend through a non-lane path container. It needs its own audit
before it is written: many verbs currently rely on "not a lane" meaning
"refuse", so widening the predicate blindly would let them address things they
have always rejected.

### D4. Event export is the residual feed, and residual-only

`SObject::resolveEventFeed()` joins `contentKind()` and `resolveEventClip()` on
the base class, for the reason those two are there: the track routes without
knowing the subclass (`sobject.h:179-229`).

`STrack::eventFeed()` gains a second source class — non-track children that
export a feed — beside its own clip set and its bubbling child tracks.

**Residual-only is not a constraint here, it is the mechanism.** A fragment
consumes nothing, so its whole feed bubbles. But the rule must be stated because
a *container* asset (a folder with its own sampler) must export **nothing** — it
already rendered those events into audio, and exporting them too would trigger
them twice. `sliveplan::midiConsumerFor` already walks routing upward to find a
consumer; MIDI-out already has "a child with its own port stops bubbling". Reuse
the predicate; do not write a third one.

### D5. Rate ≠ 1 on an event-exporting cut is REFUSED, not approximated

POSITION_DOMAINS rule 7: the tick→frame conversion happens exactly once, inside
`SMidiCut`. A *stretched* cut over a fragment containing events converts twice,
and the second conversion is frame-domain — so the part stops following tempo,
which is precisely the Ardour ≤ 6 defect the tick-native model was built to
avoid.

Refused loudly (a log line naming the rate, and the UI disabling the stretch
handle on such a clip), never silently degraded.

### D6. Channel remap follows the folder-feed convention

A placement may remap the exported events' channel, `-1` = as authored — the
same convention `midiOutChannel` uses, so the scripting API speaks one dialect.
A fragment authored on channel 10 placed on a track whose instrument wants
channel 1 is the ordinary case, not the exotic one.

### D7. A pure-event fragment builds no audio capture

`SCut::buildCapture_` treats content with no random source as container-backed
and **renders it into a snapshot on the UI thread** (`scut.cpp`, the
`isContainerBacked` branch). Over a pure-event fragment that snapshot is
guaranteed silence, bought at tens of milliseconds.

It wants the short-circuit `isLiveRecording()` already gets — and note that one
needed **four** call sites (`buildCapture_`, `ensureReader`,
`invalidateAspects`, `getPreview`). Budget for four here too.

### D8. Vertical is groups; horizontal is fragments

A fragment is **single-lane by construction**. A fragment spanning BD/SD/HH
would have to decide which instrument each stream reaches, which means it
carries routing, which means it has track identity — and D1 is gone.

Multi-lane reuse is a **folder track** holding child tracks (which already
works: shared instrument on the parent, `strack.cpp:298-321`), optionally
packaged by the existing `extract-arrangement`. "Place these N fragments
together" is a selection affordance over N single-lane fragments, not a model
feature.

**Material overlap on one lane is also a group**, i.e. child tracks — *not* take
lanes. A take lane is an either-or comping alternative (proposal 17); using it
for simultaneous material would make two sounding clips look like two candidates
for one slot.

### D9. Naming

| Verb | Means |
|---|---|
| **Pack** / **Unpack** | same-lane clips ⇄ a fragment in the asset bin, placed by reference. THIS PROPOSAL. |
| **Duplicate asset here** | mint a NEW asset (deep-copying the fragment) and repoint ONE placement to it; the original and its other placements are untouched (D2). Never "un-share". THIS PROPOSAL. |
| **Group** | folder track. Existing meaning — untouched. |
| **Package** | tracks → arrangement + asset (`extract-arrangement`). Existing. |
| **Glue** | the destructive commit: audio renders to a WAV, events merge into one sequence, sources consumed. **NOT this proposal** — see non-goals. |

"Group" is already spoken for twice (edit groups via `set-edit-group` and the
`broadcast=1` attribute on six clip actions; and "group" as a synonym for folder
track in mixer inv. 1). A third meaning on a clip selection is not available.

Default fragment name: `generateAssetName`'s `Asset N` is the existing
generator, but a random or serial name defeats the identification the tag exists
for. Prefer, in order: user-supplied → first child's source basename →
`<track name> N`.

---

## Part B — the visual model

### D10. Paint the material, not the rect

A disjoint fragment's window is mostly gaps. If the clip body is an opaque
rectangle spanning the window, a later fragment interleaving with an earlier one
paints its **empty** regions over the earlier one's **material**, and the reader
sees a hole where audio is sounding.

This is clipping, not transparency: per child clip, fill its own extent; leave
gaps unpainted so whatever is beneath shows through. It is the discipline
proposal 39 M3 already adopted for the folder-sum overlay — return false and
write nothing when nothing contributed, rather than laying a flat line down
every lane.

### D11. Z-order is latest-start-on-top, and the tiebreak is contractual

Today the clip loop walks `childLinks()` in **child order**
(`strackrndrinline.cpp:225`) — z-order is insertion order, arbitrary and
unpredictable to a user. Defining it is a win independent of everything else
here.

**The payoff:** pin the tag to the clip's **bottom-left** corner and *every tag
is guaranteed visible*. A clip's left edge can only be covered by a clip
painting above it, which by the rule starts later, i.e. strictly to the right,
and so cannot reach that pixel. No occlusion test, no reflow, no z-aware layout.

The **single exception is equal start times**, which is exactly why the tiebreak
must be specified rather than left to child order: it is the only case that can
hide a handle. **Tiebreak: child index, and nothing after it.** A child index is
a POSITION in the lane's child list, so `(startTime, childIndex)` is already a
TOTAL order.

An earlier edition of this line read "child index, then object id" and that was
wrong twice: the second key is unreachable, and an `SObject`'s "id" in this tree
is its ADDRESS (`slink.cpp` serializes it that way), so a comparison on it would
order by whatever the allocator chose and differ run to run. A tiebreak this
proposal calls CONTRACTUAL may not rest on that.

Bottom, not top, because warp markers own the top edge
(`scutrndrinline.cpp:351`). Left, not right, because the right edge of an early
clip is the first thing a later one covers — which is why the container name
drawn today at `Qt::AlignBottom | Qt::AlignRight` (`scutrndrinline.cpp:471-473`)
forfeits the invariant and moves as part of this work.

### D12. The tag is a solid opaque chip, and it replaces the existing label

Not bare text. Two reasons, and the second is the load-bearing one:

- Readability without alpha.
- **Gateability.** Proposal 39 recorded that its negative controls had to be
  *bare lanes* rather than clips, because "the anti-aliased edges of the file
  name drawn on a clip land at every luminance between the text and the clip
  body". A chip of known luminance is separable by a pixel gate; loose glyphs
  are not. Adding more text to clips without a chip makes every future pixel
  gate harder.

It is **not a second label**. Plain clips already draw a file name and container
cuts already draw `getSName()`; the tag *is* that label, promoted to
bottom-left, given a chip, with its text coming from the fragment name for
fragments and the file name for plain clips. One mechanism, two sources.

Max 12 characters.

### D13. The colour is DERIVED and the RELATION is contractual

Not the border colour: on a light lane that yields a light tag on a light body.
Follow proposal 39 M3's discipline — derive from `laneFillColor()`, make the
relation contractual (readable against the clip body, distinct from the lane
fill), **assert luminance rather than a palette**, and verify it survives every
`STrackColorModifier` state and selection.

### D14. The density ladder is mandatory and the cap is ANNOUNCED

12 characters needs ~70 px; a two-bar fragment at song zoom is a handful. Full
→ elided → chip only → nothing. Per the house rule (the level meter's
`describe()` / tooltip precedent, `SLevelMeter::MONITOR_LANES`), the cap reaches
the tooltip — silent truncation reads as "it fits".

### D15. Hit-testing must NOT follow paint order

If the tag is the drag handle and hit-testing uses paint order, an occluded
fragment's handle is swallowed by the clip above it — which removes the entire
reason for the handle.

**Test all tags first, across every clip on the lane, then bodies in z-order.**
Small rule, large failure mode; it belongs in `main/timeline/CONTRACT.md` beside
the z-order rule rather than being rediscovered as a bug.

---

## Non-goals

- **Glue** (the destructive commit). A separate verb, a separate proposal. Pack
  is the reference; Glue is the commitment; `Pack` → `Glue` composes.
- **Multi-lane fragments** (D8).
- **Automation lanes on a fragment.** A fragment has no track identity, so it
  has no `self:` targets. `cut:Gain` on a child clip's own window travels with
  that window, unchanged.
- **Nested fragments beyond what the existing cycle guard allows.** Fragments
  nest exactly as assets do and reuse `sarrangements::reaches`; no new depth
  rule.
- **Per-placement windows.** D2 — that is `make-unique`.
- **Recording into a fragment.**

---

## Traps

1. **`isPathContainer()`'s two meanings** (D3). Nine call sites agree by
   accident. Splitting after a second container kind exists means auditing them
   under load.
2. **The silent audio capture** (D7). Four call sites, per the `isLiveRecording`
   precedent — missing one costs tens of milliseconds on the UI thread per
   pure-event fragment.
3. **Double-triggering** (D4). A container asset whose container has its own
   instrument must export nothing. The failure is *audible* and looks like a
   doubled part, not like a bug.
4. **Rate ≠ 1** (D5). Silent tempo-follow loss; the symptom appears bars later.
5. **The equal-start tiebreak** (D11). The only case that hides a drag handle.
6. **The collect seam is blind to paint bugs.** Proposal 39 M2 is the precedent
   and it cost a milestone to learn: a case that reads through `collectEnvelope`
   passes on the broken binary, because that seam sits *below* the paint. Part B
   needs a **pixel** gate or it has no gate at all.
7. **`SCut::buildCapture_` renders on the UI thread.** A large fragment placed
   many times is still one capture (D2) — but only because the cut is shared.
   Any drift toward per-placement cuts multiplies it.

---

## Milestones, acceptance criteria and gates

Every milestone ends green on: `./build.sh`, `python3 tools/check_layering.py`,
`python3 tools/check_logging.py`, `ctest --test-dir smaragd/build -j4`, a
reconciled registered/run/skipped count, and **byte-identical goldens**
(`smaragd/tests/goldens/`, 16-bit PCM, `cmp`).

### M0 — Split `isPathContainer()` (pure refactor, zero behaviour change)

- **AC0.1** `isLane()` exists on `SObject`, default false, overridden true by
  `STrack` and `SStdMixer`.
- **AC0.2** Every lane-semantics reader (`ssolorules.h`, `seditgroups.h`,
  `splayheadmap.h`, `splacements.h`, `sinstrumenttracks.h`) consults `isLane()`;
  every path-descent reader (`sobjectpath.h`) keeps `isPathContainer()`.
- **AC0.3** No behaviour change: the full suite is green and both goldens are
  byte-identical.
- **Gate:** full `ctest`, `cmp` on both goldens, `grep` showing no lane-semantics
  reader still calls `isPathContainer()`.

### M1 — `SLaneFragment`

- **AC1.1** A fragment holds clip links, answers `isPathContainer()` true and
  `isLane()` false, and sums its audio children at unity through a `twTrackMix`.
- **AC1.2** It has no fader, inserts, instrument, solo, arm, meters or
  automation — asserted structurally, not by comment.
- **AC1.3** It serializes and round-trips, including an unknown child kind.
- **AC1.4** `SCut` windows it exactly as it windows a track.
- **Gate:** new `fragment_test` (ctest); `action_roundtrip_test`.

### M2 — Pack / Unpack / Make Unique, and assets over fragments

- **AC2.1** `pack-clips` over a same-lane selection produces one fragment, one
  registered cut, and one placement where the material was; one undo step.
- **AC2.2** `unpack-clips` is the exact inverse (clips restored to their
  original lane at their original times).
- **AC2.3** Placing the asset a second time yields a second `SLink` to the
  **same** cut; an edit made through one placement is visible through both.
  **Scoped by D3a**: the edit is to the shared CUT's own window, not to a clip
  nested inside the fragment — no verb can address one of those today.
- **AC2.4** `duplicate-asset-here` mints a NEW asset with a DEEP-COPIED
  fragment, repoints exactly one placement to it, and leaves the original asset
  and all its other placements untouched and still sharing.
- **AC2.4b** Sharing is never broken: an edit through an asset placed three
  times is visible through all three, before AND after an unrelated placement
  has been duplicated away. **Scoped by D3a**, as AC2.3.
- **AC2.5** A selection spanning two lanes is REFUSED with a message naming the
  lanes (D8).
- **AC2.6** The cycle guard refuses a placement that would close a reference
  cycle.
- **Gate:** qxa `fragment_pack_roundtrip`, `fragment_place_reuse`,
  `fragment_duplicate_asset`, `fragment_pack_multilane_refused`;
  `action_roundtrip_test`; `docs/ACTIONS.md` rows for all three verbs.

### M3 — Event bubbling through a placement

- **AC3.1** A fragment of event clips placed on a track with an instrument is
  audible through it; the same fragment on a track with no instrument is silent
  and not an error (matching the existing event-clip rule).
- **AC3.2** A **container** asset whose container has its own instrument exports
  nothing — the part is heard exactly once (D4).
- **AC3.3** Channel remap: `-1` is as-authored; an explicit channel rewrites the
  exported events' channel (D6).
- **AC3.4** Window gating is non-destructive and synthesises the note-off at the
  window end; a looping placement re-emits per iteration without duplicates.
- **AC3.5** A rate ≠ 1 on an event-exporting cut is refused, with a log line
  naming the rate (D5).
- **Gate:** qxa `fragment_midi_feed`, `fragment_midi_no_double_trigger`,
  `fragment_midi_channel_remap`, `fragment_midi_loop`, `fragment_rate_refused`;
  assertions via `assert-midi-events` and a rendered/captured level check, plus
  `assert-log … maxCount="0"` for the double-trigger case.

### M4 — No silent capture for a pure-event fragment

- **AC4.1** All four paths (`buildCapture_`, `ensureReader`,
  `invalidateAspects`, `getPreview`) short-circuit on a pure-event fragment.
- **AC4.2** Observable: no capture is built — asserted by log absence, in the
  shape `midi_clip_render_silent` already uses.
- **Gate:** extend `fragment_midi_feed` with `assert-log … maxCount="0"`.

### M5 — Material painting and z-order

- **AC5.1** Clips paint in start-time order, tiebreak child index then id
  (D11).
- **AC5.2** A fragment's body is clipped to its children's extents; a gap paints
  whatever is beneath (D10).
- **AC5.3** A later disjoint fragment interleaved with an earlier one erases
  none of the earlier one's material — measured in **pixels**.
- **AC5.4** Repaint cost is measured and reported, against the proposal-39
  baseline (6.11 ms with overlay / 1.78 ms without, 1200×800).
- **Gate:** a **pixel** gate in the shape of `assert-lane-overlay` — trap 6; a
  script-level gate through `collectEnvelope` would pass on the broken binary.
  New qxa `fragment_paint_disjoint`, plus a `preview_envelope_test`-style
  section recovering probes from pixels.

### M6 — The tag

- **AC6.1** Every clip carries one tag chip at bottom-left; the existing
  bottom-right container label is GONE, not duplicated (D12).
- **AC6.2** With overlapping placements, every tag is visible — the D11
  invariant, asserted in pixels including the equal-start tiebreak case.
- **AC6.3** Density ladder at four widths: full → elided → chip only → nothing;
  the cap reaches the tooltip (D14).
- **AC6.4** Colour relations hold as **luminance** assertions across selection
  and every `STrackColorModifier` state (D13).
- **Gate:** qxa `fragment_tag_visibility`, `fragment_tag_density`,
  `fragment_tag_contrast`; canvas PNG grabs.

### M7 — Hit-testing

- **AC7.1** Tags are hit-tested first across all clips on the lane, then bodies
  in z-order (D15).
- **AC7.2** Dragging an occluded fragment by its tag moves **that** fragment.
- **AC7.3** A press on a body still hits the topmost clip.
- **Gate:** qxa `fragment_tag_drag` driving the REAL mouse handlers, in the
  shape `automation_lane_gestures` uses.

### M8 — Contracts and docs

- **AC8.1** `main/objects/fragment/CONTRACT.md` exists.
- **AC8.2** `main/timeline/CONTRACT.md` gains the z-order, material-clipping,
  tag and hit-test invariants.
- **AC8.3** `main/objects/track/CONTRACT.md` gains the event-export rule;
  `main/model/CONTRACT.md` gains the `isLane()` split.
- **AC8.4** `docs/ACTIONS.md` carries `pack-clips`, `unpack-clips`,
  `make-unique`.
- **AC8.5** `CLAUDE.md` gains a section in the house "read this before touching
  it — the obvious design is wrong" shape.

---

## Sub-agent assignment

Sequential where the dependency is real, parallel where it is not.

| Wave | Milestone | Agent | May start when |
|---|---|---|---|
| 1 | M0 | sonnet-a | immediately — **blocks everything** |
| 2 | M1 | sonnet-b | M0 green |
| 3 | M2 | sonnet-c | M1 green |
| 3 | M5 | sonnet-d | M0 green (paint order is independent of the model) |
| 4 | M3 + M4 | sonnet-e | M2 green |
| 4 | M6 | sonnet-f | M5 green |
| 5 | M7 | sonnet-g | M6 green |
| 6 | M8 | sonnet-h | all green |

Rules for every agent:

- **One milestone, one commit, on `feat/lane-fragments`.** No agent starts
  before its predecessor's gate is green.
- **Never re-freeze a golden.** Both goldens are byte gates; a change that moves
  them is a bug in this proposal until proven otherwise IN WRITING, with the
  licence recorded beside the case (the B4/B5 precedent).
- **A pixel claim needs a pixel gate** (trap 6). An agent that gates Part B
  through `collectEnvelope` has gated nothing.
- **Report what was NOT gated**, in the PR body, per the house rule.
- Run the DSP-sensitive cases (`grain_*`, `exact_*`, `stress_*`, `warp_*`)
  serially and first when the change touches paint or invalidation.

---

## What this proposal will NOT gate

Named up front so a green suite does not imply coverage that does not exist:

- **Drag ergonomics.** Synthesised presses go to the handler; nothing measures
  what a hand does.
- **Pixel aesthetics.** D13 asserts a luminance *relation*, never a palette.
- **Repaint latency under load.** AC5.4 measures and reports; it does not bound.
- **Deep nesting.** Fragments inside fragments inside assets beyond two levels.
- **Real MIDI hardware** for the exported feed; the capture port is the
  measurement, as everywhere else.
- **The fragment bin's UI** beyond what the existing asset list already shows.
- **Glue** — out of scope entirely (non-goals).
