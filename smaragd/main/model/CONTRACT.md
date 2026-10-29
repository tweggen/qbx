# app/model — CONTRACT

Purpose: the document model core. SObject (the tree node: properties,
ordered SLink children, aspect page cache, IRevalidatable implementation),
SLink (placement: parent + startTime), SProject (root, sample rate,
channel count, settings, revalidator), extern-file bookkeeping, and the SObjectRenderer
interface views implement.

Public headers: app/model/{sobject,slink,sproject,sprojectprops,
ssortedobjlist,sexternfile,sexternfilelist,sfilepathref,sobjectrenderer,
sclipwindow,sroutinggraph}.h

Depends on (engine): tw/core, tw/graph, tw/pages, tw/schedule, tw/sources,
tw/events (SProject owns the twTempoMap — the single tempo authority — and
SLink's beats timebase converts through it; nothing else in the app may).
App edges: NONE — the model names no concrete object types (Phase 5) and
hosts the Phase 6 decoupling seams: sappcontext.h (the ONLY way core
modules reach the application), sdetaileditors.h (view-widget factory),
sobjectpath.h (generic index-path helpers; SObject::isPathContainer scopes
the reverse search exactly as the old STrack cast did), and splacements.h
(the placement service: rootContainer/laneAt/placementAt — the generic
resolution+validation that action code uses instead of STrack/SStdMixer
casts; lane-ness is isLane(), the active lane is activeLane).

**isPathContainer() vs isLane() (proposal 41 D3, split in M0).** Two
questions that agreed by accident because, until M1, the only two overrides
(STrack, SStdMixer) answered both true. `isPathContainer()` is PATH DESCENT
only — "does the index-path search / the placement service descend into me,
and may I be windowed as an asset" — and stays the predicate `sobjectpath.h`
and the asset-creation actions use. `isLane()` is LANE STATE — "do I carry
solo, mute, edit-group membership and arm-for-recording" — and is what
`ssolorules.h`, `seditgroups.h`, `splayheadmap.h`, and `splacements.h`'s
`laneAt()` (the placement DESTINATION resolver — `place-clip`, `move-clip`
and `pack-clips`'s own lane check all use it, and it consults ONLY `isLane()`,
never the wider predicate) consult. Proposal 41 M1's `SLaneFragment` is the
first type to answer them differently: a path container (it may be windowed
and its children path-resolved) that is emphatically not a lane (no fader,
no inserts, no instrument, no solo, no arm) — see `plan/proposed/
41_LANE_FRAGMENTS.md` D3.

**`seditgroups::expandClipPaths()` TAKES ITS ROOT NAME, REQUIRED** (QBX-245).
It resolved `splacements::rootContainer()` — the master — so a grouped clip
edit made inside an arrangement found the master's lane at that index,
read ITS edit group (usually 0, ungrouped), and broadcast to nobody: the twin
lane simply did not follow. The parameter is deliberately not defaulted,
because every caller is a verb already holding one in `pathRoot_` and a
default would let a new caller silently keep the master, which is the exact
shape of the defect. The paths it hands back are relative to THAT root, so a
composite built from them must carry the root too — see `actions/CONTRACT.md`
invariant 6, which is where that half lives.

**SOLO'S SCOPE IS A USER OPTION, AND THE RULE STAYS ONE RULE (QBX-256).**
`SProject::soloScope()` says what `ssolorules.h` applies solo ACROSS:
`Global` (the default — a solo anywhere is in force everywhere) or
`Arrangement` (the per-root rule of QBX-231/233). It is RUNTIME state pushed
in by the shell from `SOpt::SoloScope`, never serialized: `app/model` may not
read `SSettings`, and a preference inside a project file would make one user's
choice everybody's.

Three things about it belong here rather than in the header, because they are
what a reader gets wrong:

1. **Every caller still passes the container that HOLDS the lane.** QBX-233's
   census answer is unchanged, and `isLaneAudible`'s `root` parameter keeps its
   meaning; in `Global` the terms span every root, so `root` then decides
   nothing. No call site branches on the scope — the one that did would be the
   fourth copy of the rule this header exists to prevent.
2. **A caller that pre-computes the flag must use `anySoloInForce()`, not
   `anySoloInTree()`.** The per-tree primitive is right for one tree and wrong
   for the scope: in `Global` it answers false while another root holds the
   solo, and the rule then never bites. `anySoloInTree` survives for
   `fragment_test`, which asserts about one tree deliberately, and for
   `anySoloInProject`, which folds it over the roots.
3. **In `Global` the walks cross the PLACEMENT edge**, because an `SCut`'s
   content link is one of its `childLinks()` — the same reference graph
   `splayhead::detail::findRec` walks, with the same per-path guard and depth
   cap. `isLane()` is still what carries solo, so a fragment still cannot
   darken anything (proposal 41 D3 is untouched); what changed is which
   containers the walk may pass THROUGH on the way to a lane.

**`containerAt()` and the M2b widening of `placementAt()`.** `splacements.h`
also has `containerAt()` — `isPathContainer()`, the strictly WIDER predicate
— and since proposal 41 M2b, `placementAt()` (a clip PLACEMENT'S own
resolver: parent path + link index) resolves the clip's PARENT through
`containerAt()`, not `laneAt()`. This is what lets a clip-property verb
(`resize-clip`, `set-clip-volume`, `slip-clip`, …) reach a clip already
nested inside a non-lane path container such as an `SLaneFragment`, while
`placementAt()`'s FINAL check — the addressed child itself must not be a
lane — is unchanged and still uses `isLane()`, so a nested TRACK is never
returned as if it were a clip, fragment or not. **Widening `placementAt()`
this way widens every DELETION that resolves through it too** (the
production `unplace-clip`/`remove-midi-clip`, both live-only action
inverses): a clip deleted while nested in a fragment is removed from the
ONE object every placement of that asset shares, so the deletion cascades to
every placement. Decided deliberately (2026-08-21, `objects/fragment/
CONTRACT.md` inv. 6) rather than special-cased away — a deletion is a shared
edit like any other edit `placementAt()` already permitted. `laneAt()` itself
was NOT widened and stays exactly as strict as before: a clip still cannot
be MOVED into or out of a non-lane container by any verb but `pack-clips`/
`unpack-clips` (`objects/fragment/CONTRACT.md` inv. 5).

`splacements::rootNamed()` also grew an ASSET-NAME fallback in M2b: a
qualifier that names neither an arrangement nor resolves under the master
falls back to a registered asset of that name, and — only when its windowed
content answers `isPathContainer() && !isLane()` — resolves to that content
directly, so `"MyLoop:0"` addresses a fragment asset's own first child the
same way `"Drums:0"` already addresses an arrangement's. An asset over a
plain lane (a folder-track "container asset") gains NO second name this way:
that lane is already addressable by itself, and the `isLane()` check screens
it out. Arrangements always WIN a name collision, so every path that already
resolved keeps its current meaning.

## WHICH ROOT DOES IT MEAN? (QBX-242)

A project has several ROOTS: the master, one per named arrangement, one per
asset fragment. Anything turning a track POINTER into a path, or a path into a
track, answers "which root?" first, and there are exactly two spellings:

```cpp
splacements::rootNamed( project, pathRoot_ )   // ASK: the root is a parameter
splacements::rootContainer( project )          // ASSUME: the master, always
```

**The action layer is correct by construction** — about 60 verbs take the root
as a parameter. **The UI, shell and widget layer is where the master gets
assumed**, and assuming it is RIGHT only when the question is about the AUDIBLE
GRAPH: the device, the transport, the recorder's arming, the MIDI out pump, the
live monitor. Only the master is wired to the device, so for those the master is
not a default, it is the answer.

Everywhere else it is a defect, **and it never announces itself**: in an
arrangement or asset tab the path resolves to `""`, every caller returns early
on `""`, and the gesture does nothing. No crash, no log line — a dead control.
That is how QBX-230, QBX-231, QBX-232, QBX-233 and QBX-234 were each found:
by a human clicking something that did not respond.

What to reach for instead:

| the question | the answer |
|---|---|
| the SPELLING that addresses this object | `splacements::specOf()`, the inverse of `laneBySpec()` |
| the ROOT OBJECT that holds this object | `splacements::rootHolding()` (QBX-246) |
| a path that may name its own root | `splacements::laneBySpec( project, spec, rootName )` |
| a root by NAME (empty = the master) | `splacements::rootNamed()` |
| the root the ACTIVE VIEW edits | `stimeline::submitActive()` / `submitFor( view, a )` / `submitRooted()` |
| genuinely the master | keep it, and carry a marker saying why |

`specOf()` and `rootHolding()` share one walk, deliberately: the spelling the
first returns must resolve back to the same object, and the second returns the
root that spelling is qualified with. Two copies of the root order could
disagree, and the pair would then describe different roots for one object.
**Never re-derive the root by parsing `specOf()`'s string and resolving the
name again** — ask `rootHolding()`.

### Where the root LIVES, which is the other half (QBX-249)

Asking `rootNamed()` instead of assuming the master is only half of it. The
other half is WHICH MEMBER holds the answer, because two funnels read
`SAction::pathRoot_` and nothing else: `applyPropagatingRoot()` copies
`pathRoot()` onto every inverse, and `setPathRoot()` is what the arranger
(`stimeline::submitFor`), the mixer strip and `SEventPassVerbs::emitVerb`
stamp an action with. An action keeping its root in a member of its own passes
`check_root_resolution` — it does call `rootNamed()` — and is still broken at
both funnels.

`add-track` held an `arrangement_`, and it cost both halves:

* **the inverse.** `applyPropagatingRoot()` read an empty `pathRoot()`, so the
  `SRemoveTrackAction` was MASTER-rooted: **one undo of an arrangement-side
  `add-track` deleted a master lane** and left the arrangement's standing.
  Silent, because `remove-arrangement` then correctly refused the
  still-populated root (inv. 15 in `objects/mixer`), so the rest of the undo
  refused too and the project was left with the master emptied.
* **the forward gesture.** `setPathRoot()` was ignored, so "New track" in an
  arrangement tab added the lane to the MASTER while the view then looked for
  it in its own mixer.

`add-sample` (whose undo destroyed the master's clip at the same address, and
whose `writeXml` wrote the path BARE so the root did not survive
serialization) and `place-asset` had the same defect. `merge-columns` and
`set-lane-hidden` had the sibling form — a `pathRoot_` of their own SHADOWING
the base's non-virtual pair, so `readXml` wrote one member and `setPathRoot()`
the other.

All ten verbs are on `SAction::pathRoot_` now, the six send verbs that were
already correct included: **a rule with six exceptions is the shape that lets
a class come back**, and `check_pathroot` job 2 therefore has no exemptions
outside the testkit, where no root-carrying verb returns an inverse (measured)
and nothing stamps one.

### `SProject::masterRoot()`, and why the rename was necessary

It was `SProject::getRootComponent()` until QBX-242. `SObject::getRootComponent()`
is a **wholly different method** — the DSP component a lane renders through, not
the project's root container — and it accounts for most of the ~140
`getRootComponent` mentions in the tree. While the two shared a name, no grep
and no checker could separate "give me the master root" from "give me this
track's audio component", which is why the class could not be gated. The rename
is non-behavioural and the compiler verifies it in both directions.

### The gate

`tools/check_root_resolution.py`, in `ci/gates.sh`. It fails on any
`rootContainer()` / `masterRoot()` site in `smaragd/main` that does not carry

```cpp
// check_root_resolution: master -- <why the master is right here>
```

and the reason is required — a marker with no prose fails too. A site that is
known-wrong and not yet fixed carries `suspect <TICKET>` instead, which needs a
ticket key and is COUNTED AND PRINTED on every run, so the debt is a number
somebody sees. At QBX-242 that was 33 master sites and 63 suspects across
QBX-243 … QBX-246; **QBX-246 closed its own cluster and the count is now 35
master sites and 44 suspects** across QBX-243, QBX-244, QBX-245 and QBX-247.
The honest end state is zero suspects.

### Why the testkit's own seams had to go first (QBX-246)

`SActionRunner::assertTrackCount_` — the trailing `<assertions>` block every
case ends with — and `verifyUndo_` were **master-rooted**, so a case that built
an arrangement and broke it still reported the master's unchanged count as
correct. **A gate that cannot see the arrangement cannot fail for the
arrangement**, and that is the self-reinforcing part of this whole defect class:
it is why QBX-230 … QBX-234 had to be found by clicking rather than by the
suite, and why QBX-246 had to land before QBX-243, QBX-244 and QBX-245 could be
gated at all.

`assert-track-count` now takes `root=` (absent = the master) and `verify-undo`
compares **every** root's count, the root SET included — undoing an
arrangement's creation unregisters it, so a root the final state had may
legitimately be gone after a full undo and must be back after the redo.

The describe/grab/click seams on `SMainWindow` were wrong **twice over**: they
assumed the master AND parsed `trackPath` with a bare `stringToPath()`. They all
resolve `splacements::laneBySpec()` now, which peels the qualifier, resolves the
root by name and **fails closed** on a name it does not know.

**A CORRECTION TO WHAT QBX-242 WROTE HERE, measured at QBX-246.** QBX-242 said a
bare parse of `"Drums:0"` became master-rooted and so "resolved a real object in
the wrong tree and succeeded". That has not been true since proposal 45 D9:
`stringToPath` yields `SPATH_INVALID` for an unparsable component, and
`resolveByPath` returns `nullptr` on it, so a bare parse of a QUALIFIED path
**fails closed** — the seam answers nothing. Watched, with the seams reverted:
`trackPath="Drums:1"` refused rather than reading the master.

So the two halves of the class differ, and conflating them is what QBX-242 got
wrong:

| half | checker | what goes wrong |
|---|---|---|
| a QUALIFIED path parsed bare | `check_pathroot` | fails closed since D9: a **dead** gesture |
| a BARE path resolved against the master | `check_root_resolution` | index `{0}` exists in every root, so it **succeeds on the wrong object** |

The consequence for anyone writing a gate: `trackPath="Nope:0"` with
`expectReject="true"` looks like the obvious arm and **passes on the broken
build too**, because the D9 parse refuses it as well. The arm that bites is a
qualified path that must REACH its arrangement, such as `trackPath="Drums:1"`
at an index the master does not have.

**`tools/check_pathroot.py` is the same class from the parsing side** and is now
in `ci/gates.sh` too. It is TWO jobs in one checker since QBX-249, as
`check_product_name` is: job 1 is the bare parse below, job 2 is "the root
lives in `SAction::pathRoot_`" above. It had not been: it was committed on 2026-08-20 by
`fbeb4b86`, whose message reads *"...and add a gate so the class cannot
return"*, and was never added to the loop, so it never ran. Wired in by QBX-242
it failed immediately with eight violations in seven files, every one of them
introduced on or after the commit that was supposed to have gated them. **A
checker in `tools/` is not a gate until `ci/gates.sh` names it.**
volumeDbSnapshot() is the thread-safe volume read (it holds volumeMutex_,
which a bare getVolume() does not). Proposal 39 M2 left it with NO CALLER —
it deleted the paint-time fader multiply from drawObjectWaveform — and kept
it anyway, because it is the only correct way for anything off the UI thread
to read a fader and deleting the one safe reader would leave the racy one as
the only option. M3 gave it a caller again: STrack::collectChildSumEnvelope()
reads a CHILD's fader on the same paint path, which is the same race the old
multiply had, so it reads it the same safe way. (Not a contradiction of M2:
what M2 deleted was scaling a waveform by the fader of the lane it is drawn
ON. Scaling a child's contribution by the CHILD's fader is the rule, not the
exception — see main/objects/track/CONTRACT.md inv. 23.)

SEnvelopeWindow and envelopeWindowOfContext() both live in
app/model/sobjectrenderer.h: the window a paint path collects over is derived
by objects/wave (for a clip) and by objects/track (for a folder lane), and
those two may not see each other. One spelling, so a lane's overlay and the
clips on it cover the same span.
Dependency invalidation goes through the virtual
SObject::invalidateAspects() (base no-op, SCut overrides); extern-file
creation goes through SProject::registerExternFileFactory() (the wave slice
registers its WAV loader from a static initializer).

Threading: SObject follows THREADING.md rule 2 (mutex per object, snapshot
reads, atomic currentPage_); the revalidator calls the reval* delegations.
The reval pin (revalAddRef/revalRemoveRef) is a separate std::atomic —
NEVER the Qt refcount, which is main-thread-only (asserted): routing pins
through addRef()/removeRef() raced the non-atomic nRefs_ from the worker
pool and a premature deleteLater() destroyed objects that live SLinks still
referenced (the split-then-repaint vtable-garbage crash).

Invariants:
1. ~SLink must setParent(nullptr) BEFORE the vtable is torn down (childEvent
   during destruction was a use-after-destruction freeze).
2. childLinks()/childAt() are the ordered view — never iterate QObject
   children() where order matters.
3. mapTimelineToComponentPos() is identity here; only windowed objects
   (SCut) override it (POSITION_DOMAINS.md rule 3).
4. Every user-visible mutation flows through an SAction — no direct model
   pokes from views.
5. durationChanged is emitted by the OBJECT; startTimeChanged by the LINK
   (CLIP_MODEL.md — sender types differ and it matters).
6. An SLink is owned by exactly one container — its QObject parent; nothing
   adopts a caller's link (SCut builds its OWN content link from the
   SObject&; callers delete their temporary AFTER the cut exists). A parent
   must never reach the SLink QObject constructor — both ctors attach via
   setParent() as their last step, and SObject::childEvent qobject_casts
   (a non-SLink child of an SObject is ignored, never type-confused into
   childOrder_).
6b. An SLink an object OWNS but does NOT parent (STrack's reference to its
   SPluginChain is the only one) MUST be published by overriding
   SObject::ownedRefLinks(). childLinks() cannot see it, so without the
   override the reference graph is simply wrong for every walker of it —
   ~SProject's survivor ordering then put a referent in the SAME batch as
   its referrer, deleted the referent first, and the referrer's ~SLink ran
   removeRef() on freed memory (the teardown SEGFAULT after a passing
   headless run, 2026-08-16).

6c. **~SProject QUIESCES THE REVALIDATOR BEFORE IT TOUCHES THE OBJECT GRAPH**,
   on both of its paths (`shutdownRevalidation()`, first statement). The pool
   is a MEMBER, so leaving it to its own destructor joins the workers at the
   CLOSING BRACE — after the refcount cascade and the survivor pass above have
   already deleted every SObject the pool's borrowed IRevalidatable* pointers
   name. ~SCut's retireObject() is per-object and runs in the middle of that
   cascade, which keeps scheduling more work as objects invalidate on their way
   out; the up-front shutdown is the property, the retire is the belt. Symptom
   when it was missing: the app hung in ~SProject's join on File -> Open, with
   a worker faulting in Qt's per-thread teardown (workers are Qt-adopted —
   SObject::revalCompleted posts a queued invokeMethod), i.e. nowhere near the
   object that had been freed under it. See tw/schedule CONTRACT inv. 11.

7. An external file reference is PORTABLE ON DISK and ABSOLUTE IN MEMORY.
   SFilePathRef::toStored/fromStored are the only encoders, and they pick
   project-relative first, "~/..." when the climb lands exactly on the home
   directory, absolute only when it goes past home to a root (sfilepathref.h
   explains why each). The anchor is SProject::projectFilePath(), which the
   save/load actions maintain — never the serialized fileName attribute.
   Nothing in the model ever hands a stored spelling to a file API, and
   nothing hands an absolute path to a serializer.
8. addRef()/removeRef() are main-thread-only (asserted). removeRef()'s
   deleteLater() is re-checked in SObject::event(): a re-referenced
   (1→0→1) or reval-pinned object swallows the stale DeferredDelete (the
   last unpin re-arms it via deletePending_), and ~SObject warns if it ever
   runs with live references.
9. The preview NEVER moves a live play cursor. A sample-backed object reads
   its twRandomSource statelessly; a CONTAINER (no random source) reads its
   root component's FROZEN PAGES — requestPage() at FRAME_CAPACITY
   granularity, each page chained into the next as previousPage
   (FREEZE_PROTOCOL.md "Sequential consumers"). The retired spelling —
   seek() + calcOutputTo() per preview window — wrote a cursor an in-flight
   freeze on another thread was about to read (the exact race
   twComponent::seek()'s detector was installed for), from the paint path.
   One probe means the same thing either way: the signed min/max envelope
   of its previewSkip_ window, scaled to [-128,127].
9a. **A preview probe folds EVERY CHANNEL** (proposal 36 B8): the smallest min
   and the largest max across the channels, i.e. the union of the per-channel
   envelopes. It was channel 0 alone, which was harmless while nothing above
   width 1 reached a sink and is a lie now — a clip whose loud material sits on
   channel 1 would draw as the quiet one. The CONTAINER branch folds the
   channels OF THE PAGE IN HAND (§4.4), never a declared width, because an
   insert-less twPluginChain forwards its input page verbatim and its silence
   pages are width 1. The waveform stays ONE LANE and that is a decision:
   preview_t, swaveformdraw, SCut::getPreview and every inline renderer are
   single-envelope by type, and an arranger clip lane has no room to stack six
   waveforms. Per-channel LEVEL is the meter's job (SLevelMeter). The fold is
   key material: twAspect::PreviewPeaksVersion is 2, so a v1 sidecar cannot be
   adopted under the new rule.
9b. **A Preview ASPECT PAGE is not a probe array.** CapturePageData::data holds
   float samples at ~1 kHz written by CaptureRevalidator::dispatchRecomputation,
   with no probe count, hop or duration attached; its only consumer
   (SCut::getPreview) uses the page's EXISTENCE as a readiness signal and never
   reads the payload. SPlainWave::getPreview used to reinterpret_cast it to
   preview_t* — proposal 36 trap 26, settled at B8. It was UNREACHABLE (an
   SObject's currentPage_ is written only by the revalidator, and only SCut ever
   calls scheduleRevalidation), so removing it changed no behaviour; it was not
   "fixed", because a page with no geometry cannot answer a
   (start, length, nProbes) question whatever its element type.
10. There is ONE notion of how long the project is.
10. `SObject::contentKind()` says what an object's material IS (Audio or
   Event, proposal 37 D8b); the default is Audio, because everything that
   existed before event clips is. It is NOT a track kind — a track holds
   whatever clips it is given — and nothing above the clip branches on it.
   Two things consume it: `SClipWindow::wrapContent()` picks the window type
   for a piece of content, and `STakeStack` refuses a take of a different
   kind.
11. `SClipWindow` (sclipwindow.h) is the WINDOW layer of CLIP_MODEL.md as an
   INTERFACE, and it is what the windowed verbs address — split, resize,
   duplicate, unsplit, set-clip-name and the take verbs all dispatch on it
   rather than on a concrete window class (`ssplitclipaction.cpp` used to
   compare the class NAME, which no second window type could ever have
   satisfied). Two rules keep it implementable by a window whose content is
   not measured in frames: the READ api is timeline FRAMES, and a setter
   takes timeline frames and converts EXACTLY ONCE inside the implementation
   — two callers converting independently is how a rounding difference
   becomes an off-by-one clip edge. The exceptions are explicit
   (`contentAnchorExact` / `setWindowExact`), because the slip anchor is
   stored content-authoritative and must not drift under a stretch edit.
   Pitch, formants, warp anchors and the grain params are deliberately NOT
   on it: they are audio-specific, and a verb that edits one is an audio
   verb. The per-kind wrap factory is registered from the slice that owns the
   window type (static initializer, OBJECT-library rule), so the model still
   names no concrete object type.
11b. **`SLink::timebase` and the tempo map** (proposal 37 D2). `SProject`
   holds ONE `twTempoMap`; `getBPMTempo()` is a derived view of it and there is
   no second tempo scalar (a stored `60/bpm` and a stored µs/quarter disagree
   in the tenth microsecond, which lands on a frame boundary in a long
   project). It is written by exactly two callers — the `set-tempo` verb and
   the loader. An `SLink` whose timebase is `beats` carries an exact
   `startTicks` as the AUTHORITY and derives `startTime`; `setStartTime()`
   converts once and stores ticks, and `set-tempo` re-derives frames from the
   ticks, so no number is ever converted twice and repeated tempo edits cannot
   drift. Serialized only when non-default for the object's content kind, so
   every pre-36 project re-serializes byte-identically.

11c. **`resolveEventClip()` and `eventsChanged()` live on `SObject`, not on a
   MIDI type.** That is what lets `app/objects/track` route an event clip into
   its `twEventClipSet` and react to a note edit without an edge to
   `app/objects/midi` — the track consults MIDI-ness only through
   `contentKind()`. `windowTakeAt()` is the same idea for a take column: a verb
   can address a take without naming `STakeStack`.

12. There is ONE notion of how long the project is.
    SProject::getDurationFrames() is the root container's content extent —
    getRootComponent()->getDuration(), i.e. the same SObject::
    getChildrenExtent() walk the arranger already draws through
    SStdMixerView::contentDurationChanged. It is measured from position 0 to
    the LAST end (not last-minus-first), so it is directly the extent of a
    whole-project render, and it is the LAID-OUT extent: mute, solo, the
    render gate and take selection change what is audible, never how long the
    project is. An EMPTY container reports 1 frame, not 0 (SStdMixer/STrack
    floor their extent), and that sentinel is normalized to 0 here. Do not
    add a second traversal: a duration that disagreed with the one on screen
    would be worse than the hardcoded 60 s constant this replaced.

10. SProject::channels() IS THE ONE AUTHORITY ON CHANNEL WIDTH (proposal 36 M1
   for the data, B4 for the consequence).
   It persists as <SProject channels='N'>, takes only 1/2/4/6/8, and reads
   with the sampleRate warn-and-default idiom: missing (a legacy file) or
   unsupported means 2 — today's audible width — plus exactly one warning,
   because a wrong channel count corrupts a project as thoroughly as a wrong
   rate, and because a project must never fail to OPEN over it.
   Since B4 it REACHES THE GRAPH: channelsChanged is connected to
   STrack::setChannels and SStdMixer::setChannels, and it is the width of every
   page the track and master path freeze. M1's invariant here was the opposite
   ("nothing connects it to a bus count") and was guarded by a call counter,
   because an undo of 6 -> 2 that reached the old STrack::setNBusses would have
   hit its shrink Q_ASSERT_X( false, ... ). B4 retired both the per-bus
   instantiation and the counter: a width change creates and destroys nothing,
   so the shrink is ordinary. Nothing connects it to tw303aEnvironment, which
   still has no channel state. Since B5 the width also reaches the FILE and the
   DEVICE: `RenderParams::channels` derives from the project (via the root's
   declared width), so a channels='6' project renders a 6-channel file
   (`qxa.mc_six_channel`). B4's note here said a width change was inaudible;
   that was true for B4 and is not true now.

How to test: full qxa suite; action_roundtrip_test for serialization
adjacency; filepathref_test (ctest) for the three path-storage rules and
sample_path_portable.qxa for the save -> reload -> render wiring;
project_channels_test (ctest) for invariant 10 — including the assertion
that set-project-channels and its undo REACH the track's width in both
directions, the 6 -> 2 undo included (M1's version of that check asserted the
opposite and counted STrack::setNBusses calls, because M1's claim was a
negative; proposal 36 B4 made it a positive and retired the counter);
preview_container_test (ctest) for invariant 9 — probe values across page
boundaries, one reset for four pages, one reposition per page.

Known debt: none of the former model→objects edges remain; the module is
ready to become a real build target once its remaining consumers are.

**`SObject::getCapture()` NEVER SCHEDULES REVALIDATION, so only an `SCut` can
ever own an aspect page** (proposal 36 §7 trap 28; found at B8, recorded at B9,
deliberately NOT fixed). The base implementation returns `currentPage()` and
stops, with a TODO reading "Phase 5e.5 — unify CaptureRevalidator to work with
`SObject*`"; `SCut` overrides it to call `scheduleRevalidation(this, …)`, and
those two `SCut` call sites are the ONLY ones in the tree. Since `currentPage_`
is written only by `CaptureRevalidator::processRevalidationJob`, i.e. only for
an object that reached `scheduleRevalidation()`, a non-`SCut` object's page is
permanently null. Two consequences worth knowing before anyone changes it: it is
WHY trap 26 (the preview aspect page's disputed element type) stayed latent long
enough to be discovered by reading rather than by a wrong waveform — the
disagreeing reader lived on `SPlainWave`, which can never have a page — and
**anyone wiring a second revalidatable object type makes that whole area live at
once**, including a payload with no probe geometry attached. Give the page a
geometry before giving it a second producer.

## Inline `<automation>` (proposal 37 P5, design §3.3)

**`<automation>` is a sanctioned NON-`SLink` payload of a known element, and the
loader ignores it for ordering.** `SObject` owns the lane vector, emits the
element from `SObject::serialize()` (and every override that writes its own
children must call `serializeAutomation()` itself — `SPluginSlot` does, next to
`<state>`), and reads it back in `readPostChildrenAttributes()`. Writing NOTHING
when there are no lanes is load-bearing: it is what keeps every project written
before P5 byte-unchanged.

- **Lanes live on `SObject`, not on the four owner types.** A verb, the
  serializer and the testkit all have to reach a lane without knowing which
  object slice its owner belongs to, and `main/actions` may not depend on
  `objects/*` at all — the same argument that put `contentKind()` and
  `resolveEventClip()` here. WHICH targets are legal on WHICH owner is validated
  by the verbs, not by the storage.
- **The lane vector is MAIN-THREAD ONLY.** Every verb, the loader and the
  serializer run there. What crosses to a freeze thread is the immutable
  `twAutomationCurve` SNAPSHOT, handed to the consuming component under ITS
  mutex and read once per page into a local (THREADING rule 2).
- **`onAutomationChanged(lane, start, end)`** is the hook an owner overrides to
  push the new snapshot into its engine components and stale exactly that range;
  the default does the `invalidateRenderPathRange` and nothing else.
  **`applyAutomationToEngine()`** is the load-path replay, because the lanes are
  read before the components exist.

## The generic TAKE-COLUMN seam (proposal 21 L4)

`SObject::windowTakeAt()` has existed since proposal 37 D8b so that a verb could
ADDRESS a take without naming `STakeStack`. L4 needed to BUILD one from
`objects/midi`, which sits at the rank of `objects/cut` and must not depend on
it, so the seam grew the four calls it was missing —
`windowTakeCount()`, `activeWindowTakeIndex()`, `insertWindowTake()`,
`removeWindowTake()`, `setActiveWindowTake()` — plus a registered factory pair
on `SClipWindow`:

    registerTakeColumnFactory( wrap, collapse )
    wrapIntoTakeColumn( project, lane, clipLink )   // plain placement -> column
    collapseTakeColumn( lane, columnLink )          // one-take column -> plain

Registration is by static initializer from the slice that OWNS `STakeStack`
(`stakehelpers.cpp`), exactly like `registerWrapFactory` — and it works for the
same reason: `smaragd_app` is an OBJECT library, so nothing strips a translation
unit whose only reference is a static constructor.

Every override on `STakeStack` is a ONE-LINE FORWARDER. The stack has been
window-typed since D8b; the two helpers already took an `SClipWindow` and named
no `SCut`. What was missing was not generality but REACH, and this is the
smallest thing that provides it. The defaults (`0` / `-1` / null / no-op) mean
an object that is not a column answers honestly rather than being asked to
pretend, so a caller's `windowTakeCount() == 0` test is "this is a plain
placement" and needs no `dynamic_cast`.

### `splayheadmap.h` — where an arrangement is being heard (proposal 09 §15)

`splayhead::derivedPos( project, rootName, masterPos )` walks the transport's
position DOWN to one arrangement root: child links by start time, a span test
at every placement, and `SObject::windowStep()` at every window.

It lives in the model, and reaches a cut's slip/loop/warp without depending on
`app/objects/cut`, for exactly the reason `contentKind()` and
`resolveEventClip()` do — the virtual is the seam.

Three properties, each a decision (proposal 09 D23):

1. **First hit in child-link order wins** when an arrangement is placed more
   than once, or when placements overlap. One cursor, chosen deterministically.
2. **`sounding == false` is an answer, not a zero.** `pos` must not be read as
   a position when it is false.
3. **The cycle guard is per PATH, not a global memo.** The same body reached
   twice at two positions must be visited twice — the first reach may not
   contain the position while the second does. Depth is bounded too: a cycle
   here is a stack overflow on the UI thread.

Main thread only, and NON-BLOCKING: a repaint calls it.

## The TAKE terminals on `SObjectRenderer` (take lanes)

```cpp
virtual void drawTake( SLink &, SRenderContext &, int takeIndex );
virtual bool collectTakeEnvelope( SLink &, const SEnvelopeWindow &,
                                  preview_t *, int takeIndex );
```

`draw()` / `collectEnvelope()` for ONE TAKE of a take column rather than for
whichever take is audible. **`takeIndex < 0` means "the audible one"**, and the
DEFAULTS forward to `draw()` / `collectEnvelope()` — so every renderer that is
not a take column is byte-for-byte what it was, and no golden and no existing
case moved.

They exist so a TAKE LANE is painted by the same walk the composite lane uses —
same domain map, same loop tiling, same delegation — with only the terminal
swapped. See `main/timeline/CONTRACT.md` inv. 34 for the defect that made this
necessary.

Two implementations, and the asymmetry between them is deliberate:

* `STakeStackRendererInline` delegates to the NAMED take instead of the active
  one, with the same link and the same context. At `takeIndex < 0` it is
  literally `draw()` / `collectEnvelope()`.
* `SCutRendererInline` walks its own window (slip, stretch/warp, loop tiling)
  and, when the CONTENT is a take column, terminates in the content renderer's
  `drawTake` / `collectTakeEnvelope`. Whether the content is a column is asked
  through the GENERIC seam (`SObject::windowTakeCount() > 0`), never a cast —
  `objects/cut`'s renderer must not know which slice implements a column.
  It paints SEGMENTS ONLY: no pitch badge, no warp handles, no gain envelope.
  Those belong to the wrapper, and the take draws its own inside the segment.

**`collectTakeEnvelope(..., -1)` on a CUT WRAPPING A COLUMN is NOT byte-identical
to `collectEnvelope`, and that is correct rather than a leak.** The wrapper's
`collectEnvelope` reads the CAPTURE (`SCut::getPreview` -> `capPeaks_`,
asynchronously built and `capPeakSkip_`-quantised); the take terminal reads the
take's own live preview. They agree POSITIONALLY — which is the whole point —
and not sample for sample. The consequence to know rather than rediscover: after
this change the composite lane and the take lanes of a WRAPPED column derive
from two different data paths, so where the capture is missing the composite
still shows "Asset: (no preview)" while the take lanes draw. The take rows agree
with EACH OTHER, which is what comping needs. Byte identity holds only for the
DIRECT shape.

## `fillBodyByMaterial()` — one clip-body rule, two lanes

A header-inline template beside `envelopeWindowOfContext()`. Fill a body rect's
COLUMNS only where a caller-supplied collect reports material; leave gap columns
untouched; return false and paint nothing when there is no envelope, so the
caller does the original solid fill. Shared by `STrackRendererInline::draw()`
(the composite lane) and `SMVActualView::drawTakeLane()` (the take lanes) —
see `main/timeline/CONTRACT.md` inv. 35.

It lives here because it is about a RENDERER and an `SRenderContext` and belongs
beside them. (Not for `envelopeWindowOfContext`'s layering reason: `main/timeline`
DOES include `app/objects/track/strackrndrinline.h`, so that header would have
worked too.)

## Missing external files, and "self-contained" (2026-08-22)

Three additions to `SProject`, all about what a project REFERENCES rather than
about what it sounds like.

**`beginLoad()` / `endLoad()` / `isLoading()` / `noteMissingFile()` /
`missingFiles()`.** While a load is in progress a sample that will not load is
RECORDED rather than announced: `SPlainWave::setWave` raises no dialog, the
loader keeps a MISSING PLACEHOLDER (`main/objects/wave/CONTRACT.md`), and the
shell reports the whole set ONCE afterwards, by name. What this replaced was one
anonymous modal `"Unable to load file."` per miss, raised from deep inside the
loader — three unreachable samples meant three dialogs and not one path between
them, so the single thing the user needed was the single thing not said.

Both spellings are kept per entry. `stored` is what the .qxp says (the portable
form — `sfilepathref.h`), `resolved` is the absolute path searched on THIS
machine. A user whose project travels between machines needs both to see which
of the two is wrong; either alone is a riddle.

`missingFiles()` is cleared by `beginLoad()`, so what a caller reads afterwards
always describes the load that just finished.

**`isMissing()` lives on `SObject`, not on `SExternFile`.** Same rule as
`contentKind()`, `resolveEventClip()` and `isLiveRecording()`: `objects/cut` has
to ask this about its content without knowing which slice that content belongs
to, and it has no edge to `objects/wave`. It is load-bearing rather than
cosmetic — see `SCut::buildCapture_`.

**`externalMediaPaths()` is the "Project is not self-contained" condition.**
Every extern file whose absolute path is not the project file's own directory
OR A SUBDIRECTORY OF IT. Two things are contractual:

* **A subdirectory counts as INSIDE.** `<projectdir>/media/` is where both the
  media browser's drop and "Collect external media" put things, so a rule that
  looked only at the directory itself would leave the banner permanently lit and
  the button unable to clear it. The trailing separator in the comparison is
  what stops `/x/proj2` matching a project in `/x/proj`.
* **An untitled project reports EMPTY.** There is no folder for anything to be
  outside OF, so the warning would be unanswerable (the button has no
  destination) as well as untrue.

MISSING placeholders ARE counted: a file this machine cannot see is by
definition not inside the project folder.

**`relocateExternFile( from, to )`** re-points an extern file at a different
path holding the SAME BYTES — what a collect produces. It rekeys
`externFileDict_` (the project's only index, and `~SPlainWave` deregisters by
its own CURRENT name, so a stale key would dangle at teardown) and announces the
move as `externFileRemoved` + `externFileAdded`, because there is no rename
signal and inventing one would make every consumer learn about a case that
happens once per collect. It deliberately does NOT reload: a copy is not a
different sample, so the resident data, its content hash and every sidecar keyed
on that hash stay valid.

Gates: `collect_external_media.qxa`, `collect_external_media_missing.qxa`,
`missing_sample_reference_verbatim.qxa`, `sample_missing_survives.qxa`,
`load_missing_sample_placed_survives.qxa`.

**A KNOWN LIMITATION, pre-existing and NOT fixed here:** `load-project` loads
INTO the existing `SProject` and does not clear `externFileDict_`, so a SECOND
scripted load in one .qxa is counted against the first load's files as well. The
GUI never sees it (`SMainWindow::openProjectFile` builds a fresh `SProject` per
open). It is why the two collect cases are separate case FILES rather than two
phases of one.

## The generic take seam is TOTAL, and it answers a different question from
## `SClipWindow::parametersOf()`

`SObject`'s take-column seam (`windowTakeAt` / `windowTakeCount` /
`activeWindowTakeIndex` / `insertWindowTake` / `removeWindowTake` /
`setActiveWindowTake`) exists so a verb can address a take without naming
`STakeStack`. Until proposal 42 M2 only `STakeStack` implemented it, so on the
WRAPPED shape (`SLink -> SCut/SMidiCut -> STakeStack`) every consumer got
`SObject`'s base answer — 0 / null / nothing — and silently addressed the
WRAPPER. `SCut` and `SMidiCut` now FORWARD all six one level into a take-column
content: a window over a column is a placement of that column and answers for
it.

**Two questions, two spellings, and they differ exactly on a wrapped column:**

| you want | ask |
|---|---|
| take k of this placement's column (k < 0 = the audible one) | `SObject::windowTakeAt( k )` |
| the window whose PARAMETERS this placement carries | `SClipWindow::parametersOf( obj )` |

`parametersOf` is the placement's own window when it has one, and a direct
column's ACTIVE TAKE when it does not. It is what a clip's slip, length, gain
and volume live on — so `STrack::refreshClipGainCurves` and
`SLaneFragment`'s twin use it, not the seam: a wrapped column's own clip gain
is on the WRAPPER (that is what `set-clip-volume` edits there), and reading the
take's instead would make the wrapper's inaudible.

Three call sites deliberately do NOT take the forwarded answer:

* `SMVActualView::tryOpenContainerClip` unwraps a DIRECTLY-PLACED column only —
  its rule 2 is "the content is a take stack", which never fires if the wrapper
  has already been unwrapped to its take;
* `assert-clip-window` / `assert-clip-mix` use the seam only when a `take=` is
  NAMED, and `parametersOf` otherwise — without that split, an assertion about
  the clip silently became one about its active take.

Gate: `take_seam_through_window.qxa`, over a fixture whose wrapper, take 0 and
take 1 all carry DIFFERENT windows, so no assertion can pass by accident.

## THE CLIP PALETTE — one authority, sixteen anchors, everything else derived

`app/model/sclipcolors.h` decides what colour a clip is, and it lives at the
BOTTOM of the app layering on purpose: everything that needs the answer is
above it and none of those may see each other — `app/objects/track` paints the
composite lane, `app/objects/wave` and `app/objects/midi` paint the content on
it, `app/timeline` paints the take lanes, and `app/shell` MEASURES all three.

**A track carries an INDEX; only the anchor is stored.** The selected body, the
muted body, the waveform colour and the CUT MARK are HSL moves off that one
anchor (`body()` / `wave()` / `cutMark()`), never separate constants. So the
picker this is built for — any colour, later — gets its whole family for free,
and there is no way for one mount to invent a "bright" variant another mount
disagrees with.

`cutMark()` (plan 53 §4.4, QBX-157) is the body one step DARKER, and the
direction is the specification: the thing a cut mark is drawn over is the
WAVEFORM, which sits at L 0.80, so a lighter mark would vanish into exactly the
content it annotates. The step is a shared constant and is small on purpose —
a mark the colour of the near-black lane fill would read as a hole in the clip,
which is the one thing a cut is not. It resolves in the SAME state as the body
under it (selected first, then muted), because the pair is drawn together and
only a mismatched pair could come out lighter than its own clip.

| Rule | Why |
|---|---|
| **`SObject::colorIndex_`, -1 = AUTO**, resolved to the lane's position in the flattened lane order | On `SObject` rather than on `STrack` for the reason `contentKind()` and `resolveEventClip()` are: the serializer, a future picker action and the pixel gates must reach it without knowing which slice owns the lane. |
| It is written to the file **only when it is not -1** | Every project saved before the palette — and every committed golden — serializes byte-unchanged. The same discipline as `editGroup`, `midiRouting` and `recordingChannels`. |
| The resolved pair travels on **`SRenderContext::clipColors()`** | A wave, a cut and an event clip all draw in their track's colour while none of them may include `app/objects/track`. Whoever fills the body sets it; every nested `InlineRenderContext` inherits its parent's. |
| The auto index is resolved **ONCE PER LANE**, never per clip | It walks the project's lanes. Per clip that is O(lanes x clips) on every repaint. |
| **THE PIXEL GATES CALL THE SAME FUNCTIONS THE PAINTER DOES** | `assert-lane-overlay` and `assert-take-lane` classify a grabbed lane by exact colour and by luminance band, and they used to hardcode `QColor(160,160,160)` as "the clip body" — true only while every clip in every project was one grey. `smainwindow.cpp`'s `sClipBodyOf()` and `sClipCutMarkOf()` are the shared spellings, exactly as proposal 41 M7 gave paint and hit-test one `tagChipRect()`. |
| The CUT MARK colour is taken **OUT of `describeTakeLane`'s waveform histogram** | `waveRgb` is "the most frequent colour that is neither body nor fill", and on a short clip with several marks that is a description the marks could win — `waveMeanPct` would then measure the marks and go blind to the waveform. |
| The anchors are **mid-tone and quiet** (S 20-44 %, L 36-48 %), ordered so NEIGHBOURS DIFFER IN HUE | The selected variant is the same hue made lighter, so an anchor near white would have nowhere to go. And the auto assignment is by lane order: a hue RAMP would make two lanes stacked on top of each other nearly identical. |

**A luminance BAND is no longer a safe way to describe a third colour.** The
"overlay" band in `assert-lane-overlay` is `lumFill < lum < lumClip`, and the
clip body dropped from 160 to ~102 when it became a track colour — so the
feel-flow heatmap's own LUT, which used to sit inside that band, now sits ABOVE
it (measured: 8854 pixels in `lighterThanClip`, 380 left in the band). That
gate's load-bearing assertions are exact-colour LUT membership and are immune;
its informational `minPixels` was retired rather than retuned. Anything new
that wants to be found by colour should be found by IDENTITY.
### `SObject::clipFade()` is a base-class seam, for the layering reason

The per-clip mix funnel (`STrack::refreshClipGainCurves`) reads three factors
off each clip: its `cut:Gain` curve, its static volume and its FADE. The first
two already came through `SObject`; the fade was a `dynamic_cast<SCut*>` one
line below them, which made `objects/track` depend on `objects/cut` -- an edge
the layering gate rejects, and it rejected it on `main` for three merges before
anyone ran the gate.

`clipFade()` is virtual on `SObject`, defaults to a default-constructed
`twClipFade` (no fade) and is overridden only by `SCut`. Same shape, and the
same reason, as `contentKind()` and `resolveEventClip()`: the track must read
the property without knowing which window slice is in front of it.

It is deliberately NOT on `SClipWindow`. A fade is AUDIO-specific, and that
interface's contract already lists pitch, warp anchors and the grain params as
absent for exactly that reason.

### `sdefaultreset` — the double-click-to-default gesture, and why it lives here

`app/model/sdefaultreset.h` installs one event filter that runs a caller-
supplied restore on a left `MouseButtonDblClick`. It is in the LOWEST app
layer for the same reason `sclipcolors.h` and `sclipwindowgeometry.h` are:
`app/timeline` (the Track Detail fader, the Clip Detail fields) and
`app/pluginui` (the generic parameter sliders) both need it and are peers in
`app_ui` that may not include each other in that direction. It reaches nothing
in the model — it is a Qt event filter and nothing more.

It takes a `std::function`, not a value, and that is not generality for its own
sake: the three call sites commit in three different ways (a slider's
`valueChanged`; a quantising handler that may be inside an automation write
pass; a spin box that commits on `editingFinished` ALONE). A helper that wrote
the value itself would silently no-op on the third — the field would show the
default and the model would keep the old value.

It also watches a spin box's embedded `QLineEdit`, because `QAbstractSpinBox`
never sees a double-click over its own text: the child line edit consumes it,
so filtering the spin box alone leaves the gesture working on the arrows and
dead over the number. The cost is that double-click-to-select-a-word inside
those fields becomes reset-to-default; that is the requested trade, and the
text is still selectable by dragging, by Ctrl-A and by tabbing in.

And it SWALLOWS the event. On a QSlider the second press would otherwise start
another drag or jump the handle to the click position, moving the control off
the default it was just asked for.

### `suifonts` — the NAMED UI fonts (QBX-102, QBX-115, QBX-201)

`app/model/suifonts.h` spells the only UI fonts that other widgets are
required to MATCH, so "the same size as" is one function rather than a set of
point sizes that happen to agree:

| Font | Is | Used by |
|---|---|---|
| `smallFont()` | the application font at `SMALL_POINT_SIZE` (9 pt) — the arranger track head's NAME font | the track head's name and volume labels |
| `treeFont()` | `QApplication::font("QDockWidgetTitle")` — the font `QDockWidget` draws its TITLE in (its own `init()` sets the title font from that class key) | `SExternFileList`, the media browser's tree, `SMixerStrip` (set on the whole strip before its children exist) and `SPluginEffectStrip` (both mounts) — **both moved here from `smallFont()` by QBX-115**, which asked for the mixer to be DENSE rather than to agree with the track head |
| `denseFont()` | `treeFont()` one point SMALLER, floored at `DENSE_MIN_POINT_SIZE` (7 pt) | the plugin NAME on a `SPluginEffectStrip` row in COMPACT mode (QBX-201) |

`denseFont()` is a DERIVATION and not a third absolute size, which is the
whole of why it can be asserted: the requirement is "smaller than the row it
sits in", and an absolute 8 pt would be LARGER than the row on a platform whose
dock-title font is smaller than that. `describeSlot()` reports the relation
(`nameSmaller=1`), never a number.

The tree font is the PLATFORM THEME's: macOS supplies a smaller dock-title
font, while Windows and Linux supply none, so there it IS the application font.
**Consequence to know:** on Windows and Linux a list that never called
`setFont()` "equals" the tree font by default, and on this repository's Linux
box the application font is already 9 pt, so every size and equality check
passes with no `setFont()` at all. That is why `assert-ui-fonts` also reports
`<mount>Set` (`Qt::WA_SetFont`), and why that field is what the gate asserts.
It lives in the lowest app layer for the `sdefaultreset` reason: timeline,
mixerui, pluginui and mediabrowser all need it and cannot see each other.

## System lanes and their addressing (proposal 45 D1/D8/D9)

Three predicates on `SObject`, all on the base class for the reason
`contentKind()`, `resolveEventClip()`, `isMissing()` and `isLaneFragment()` are
— `app/model` itself has to ask them. The placement service (`splacements`) and
the path resolver (`sobjectpath.h`) sit BELOW `app/objects/track` and cannot see
a track policy header, and they are exactly the two that must know.

- **`systemRole()`** — `None` / `Master` / `Send` / `Conductor`. Never spelled
  as a conjunction of existing predicates: "a lane that is not among its root's
  `childLinks()`" happens to describe the master lane and nothing else, and
  relying on an accidental agreement between two predicates is what proposal 41
  M0 split `isPathContainer()`/`isLane()` up to stop doing.
- **`isHidden()`** — VIEW state, serialized only when true. **Never an audio
  property**: a hidden master lane is fully in the signal path. Worth saying out
  loud, because `isLiveOwnedLane()`, `ssolo::isLaneAudible()` and
  `STrack::isCollapsed()` are three existing flags that look like they might
  mean "not heard" and do not.
- **`acceptsClips()`** — the placement policy, consulted at one narrow seam and
  deliberately NOT at `splacements::laneAt()`, which has 71 call sites across 45
  files and is the general lane resolver for `set-track-volume`, `arm-track` and
  the automation verbs. A refusal there would break the master fader.

**The sentinel.** A system lane is not among its root's `childLinks()`, so no
non-negative index reaches it. It is addressed by a NEGATIVE step —
`SPATH_MASTER` (-1), with `-2 - k` reserved for sends — which keeps a path a
plain `QList<int>`: every action storing one as a member, every serialized
`trackPath` and every existing call site is unchanged. Resolution goes through
`SObject::systemLaneAt()` / `systemLaneSentinelOf()`, so this layer needs no
knowledge of what a mixer is.

Both directions are load-bearing:

- **The write side would ship broken.** `strackpath::pathOf()` walks
  `childLinks()`, so without its sentinel branch it answers `{}` for a system
  lane — which is ALSO the address of "the root itself". Every track head
  derives its commit address that way. No assertion about a lane's own
  PROPERTIES can see this; only a `pathOf()` round trip can.
- **The read side fails CLOSED.** `QString::toInt()` answers 0 for any
  non-numeric text, so before the sentinel `"$master"` resolved silently to the
  FIRST USER TRACK. An unparsable component is now `SPATH_INVALID` and resolves
  to nothing. That change immediately found three fossils: three plugin cases
  addressed `trackPath="/mixer/0"`, a spelling that exists nowhere in the code
  and only ever worked because `toInt()` answered 0.

**A negative step can never index out of bounds.** Every path consumer in
`main/` indexes through `SObject::childAt(int)`, which returns nullptr for a
negative index, so a sentinel or an `SPATH_INVALID` reaching an ordinary
indexing site fails closed at that one accessor.

**A send name may not be spelled with a colon.** `parseQualified` splits the
root qualifier on the FIRST ':' (`"Drums:0"`), so `$send:<name>` would be parsed
as root `$send`. The reserved surface spelling is index-based (`$send0`) until
proposal 45 M7 decides the name form.

---

## The SEND TAP (proposal 47 M0)

`SSendTap` (`app/model/ssendtap.h`) is `{ dest, levelDb, preFader, enabled }`,
and the vector of them lives on **`SObject`**, not on `STrack`.

Same argument as `contentKind()`, `resolveEventClip()` and the automation-lane
vector: the serializer, the verbs and the testkit must reach a tap without
knowing which object slice owns it. WHICH objects may legally carry one is the
verbs' business, not this class's — an audio tap on a lane that has no audio is
refused where it can be ANNOUNCED, never by the type of the member.

**THE DESTINATION IS A NAME, NEVER THE `-2-k` SENTINEL.** Proposal 45 already
restricts `remove-send-lane` to the LAST lane precisely because that sentinel
IS the address, so removing one from the middle re-points every path naming a
later lane. A stored index would carry the same defect with none of that
protection. The name is also the user-facing address (`$send:Reverb`), and
`midiOutPort` is the precedent: a portable NAME in the file, the machine-local
id looked up.

**Serialized INLINE as `<sends>`, and written only when the list is
non-empty** — the automation-lane discipline (37 P5). An inline child of a
known element is invisible to the loader's `<SLink>`-based ordering and is
ignored by an OLDER build, and writing nothing when there is nothing is what
keeps every project written before proposal 47 and both committed goldens
byte-unchanged.

**`setSendTap()` REPLACES a tap that already addresses the same destination.**
Two taps from one source into one bus would sum that source twice, which is not
a thing a user can mean; a duplicate destination in a file collapses to one for
the same reason.

**`enabled` is written unconditionally**, unlike every other non-default-only
attribute in this tree. It is the one field whose ABSENCE would have to mean
"true", and a reader that infers a default from a missing attribute is how a
disabled send comes back ON after a round trip.

**A serialize() override must repeat `serializeSends( o )`.** `STrack::serialize`
mirrors `SObject::serialize` body-for-body rather than hooking into it, so an
inline child that is not repeated there is silently dropped on save with no
error anywhere — gated by `qxa.send_tap_model`'s round trip, and watched
failing by deleting exactly that one call.

## The ROUTING GRAPH type and the routing hook on the invalidation walk (proposal 55 M0)

`app/model/sroutinggraph.h` is a PURE graph over opaque node identities
(`sroute::Node` is `const void *`, never dereferenced) with four edge kinds:
containment, send, aux, key. `add()` takes any edge; `accept()` takes one only
when it closes no cycle among the edges already in the graph (a self edge is a
cycle of one); `acceptInOrder()` is the mixer pass's whole break algorithm,
spelled once so `routinggraph_test` drives exactly what the pass runs. It lives
here because both the verbs and the pass (`objects/mixer`) must see it and
naming no object type keeps it legal in this layer.

**The invalidation walk reports what it bumped.**
`invalidateRenderChainsContaining( target, bumped )` and its range twin append
every object whose chain they bumped (with the ranges in that object's own
domain), and `invalidateRenderPath[Range]()` hands the list to the ROOT's
`invalidateRoutingDependents( bumped, whole )`. The default does nothing; only
a root that owns routing (`SStdMixer`, `objects/mixer` inv. 29) overrides it.
When the walk cannot find the edited object and falls to `renderPathOwner()`
(45 D11, a system lane), the owner is handed the object itself as well, so a
send lane's OWN routing dependents are reached from an edit on that lane.

**Proposal 55 M2 adds two virtuals beside it.** `routingInputsChanged()` is
asked of the root that holds a lane (`splacements::rootHolding`) when
something a routing edge depends on changed shape -- the mixer re-runs its
pass; and `isReturnLane()` lets `splacements::placementLaneAt()` word its
refusal for a return track ("is a RETURN track ... carries no clips"), which a
case must be able to tell from proposal 45 D6's system-lane message.

## The recorder's TAKE NAME, and the one test for "this take came from a recording" (plan 53 §8 P4, D5, QBX-182)

`app/model/stakenames.h` — `recorded( n, stamp )` builds `"Take 3 · 14:27"` and
`isRecorded( name )` answers whether a name carries that stamp. Header-only, two
functions, and it is in `model` although every consumer so far is a `cut` verb:
`place-midi-recording` names an EVENT take through the same format and
`objects/midi` may not depend on `objects/cut`. A take name is a model-level
string convention, not an audio one.

Three things read it. The USER, on the clip body and in the take lane.
`select-take take="newest"` (§6.5), which needs to tell a recorded take from one
that came from the user's arrangement because "a take without a stamp is older
than any with one". And `cleanup-takes` (§6.3, decision D5 (a)), which exempts
takes that did NOT come from a recording — `isRecorded()` is that origin marker.

**IT IS A HEURISTIC AND IS MEANT TO BE ONE.** A user may type
`"Take 2 · 14:27"` by hand and it will read as a recording. The alternative is a
serialized per-take origin flag, which is a file-format change D5 did not ask
for, and the cost of a false positive is that `cleanup-takes` offers to remove
one clip the user would have kept — which its confirmation lists by name.

**ORDER IS NOT READ OFF THE STAMP.** `HH:MM` has no date and two passes in one
minute tie, so `newest` orders by the take's INDEX in its column, which is
recording order because a recorded take is always appended (`merge-columns`
names the pass as its final operand, `add-take` appends). The stamp says
WHETHER, the index says WHICH.

## The PLAYBACK AUTO-FADE: where its policy lives, and why here (plan 53 §8 P5, D3a/D3b, QBX-183)

`app/model/sautofade.h` is the whole policy of the project auto-fade: the
ms-to-frames conversion, the per-edge rule, the SEAMLESS test, the per-lane
walk that decides which joins are seamless, and the re-apply that makes a
change to the property audible. Nothing about it is in either consumer.

**IT HAS TWO CONSUMERS AND THEY CANNOT SEE EACH OTHER.** `objects/track`
applies it at CLIP edges, in `STrack::refreshClipGainCurves`; `objects/cut`
applies it at COMP boundaries, in `STakeStack::ensureCompColumn`. There is no
edge between those slices and there will not be — it is the same rule that put
`clipFade()` and `contentKind()` on `SObject` as base-class seams. So the one
place both can reach is here, which is also the lowest place that can hold it.

**AND IT HAS A THIRD READER THAT IS NOT A CONSUMER.** The Clip Properties
panel shows the user which of a clip's edges the auto-fade skips (§6.2's
"showing exempt edges"), and it reads `laneSeams`, the same function
`refreshClipGainCurves` hands to the mix. A second implementation of that
question would be wrong SILENTLY: a status line that disagreed with the audio
looks exactly like one that is right.

**THE SEAMLESS TEST NEEDS NOTHING AUDIO-SPECIFIC**, which is what makes this
possible at all. It compares the content object, the loop state, the time
scaling and the source position at the join — and `SClipWindow` publishes all
four, because they are the arithmetic every windowed verb needs. The warp
anchors are NOT compared: the window interface deliberately does not publish
them, and it costs nothing that matters, because `timelineToSourceExact` is
map-aware on both sides so the POSITION test is exact under any warp, and
every edit that makes a seam copies the anchor list wholesale.

**THE READ DEFAULT IS 0 AND THE SEED IS 3 ms, AND THAT IS NOT A DETAIL.**
`SProject::properties_` is initialised from `SProjectProps::defaults()` and a
load MERGES the file's keys over it, so a value in `defaults()` is what EVERY
PROJECT EVER SAVED reads — no file written before plan 53 carries the key.
Putting 3 ms there (D3a option (b)) would move every byte-identity golden and
every per-second RMS gate in the suite. So `autoFadeMsDefault()` is 0,
`autoFadeMsSeed()` is 3.0, and `seedInto` is called ONLY where a project is
created with no file behind it: `SMainWindow`'s fileNew path and
`close-project`'s fresh project. Never after a load — the rule
`app/shell/srecordmodeseed.h` states at length for the record mode, for the
same reason and with the same consequence if broken.

**IT READS DURATIONS WITH THE BLOCKING FORM**, and so does its caller for the
placement length. `getDuration()` / `duration()` take the try-lock snapshot and
fall back to `lastGoodSnapshot_`, which under background-worker mutex
contention answers with a PRE-EDIT duration (proposal 19 Phase 2b). That is the
`takes_group_broadcast` flake exactly: a stale duration propagated into
`twTrackMix::updateClip` and left clip windows wrong. Here it would make the
abutment test and the clamp answer about a length the clip no longer has, so a
fade would appear or vanish for one refresh -- the same shape, in the audio.
Blocking is safe because `refreshClipGainCurves` runs on the MAIN thread by
contract (it is the one main-thread funnel a clip's gain travels) and never on
the RT thread; the full suite, whose race-hunting cases run eight revalidation
workers, was re-run after the switch with no hang and no change in wall time.

### Known debt

`laneSeams` is O(n log n) in a lane's clips and is recomputed on every
`refreshClipGainCurves`, which runs on every edit that reaches the track. That
is the same cost profile the function already had for the gain curves, and the
sort is the only new part of it; a lane with thousands of clips would want the
answer cached against the lane's child-order epoch. Not done, because nothing
measured says it matters yet.

## The recorder's capture-file NAME CONVENTION, and the unused-recordings scan (plan 53 §8 P6, §6.3, QBX-184)

`app/model/srecordingfiles.h` owns two things: the shape of a capture file's
name (`<stamp>_track<n>.wav`), and the scan that decides which recordings a
project does not use.

**THE NAME IS DEFINED ONCE BECAUSE TWO PLACES MUST AGREE ABOUT IT.** The
recorder WRITES those names and the scan RECOGNISES them; before this the
pattern was a `QStringLiteral` inside `SAudioRecorder::start()` and nothing
else knew it. A scan with its own copy of the format string would be a second
definition of one fact, and the failure when they drifted would be silent —
orphan files simply not reported. `SAudioRecorder` composes through
`srecfiles::captureName` now.

The recogniser is ANCHORED AND STRICT, every field's width fixed. Offering to
bin a file the user named themselves is the one mistake this feature may not
make, so `guitar.wav`, `mix_track1.wav` and `…track0.wav.bak` are all not
captures.

**"UNUSED" HAS TWO SOURCES and they are different questions.** An extern file
the project holds that NO CLIP PLACES is the model side; a capture file in the
project's directory that no extern file references is the same thing seen from
disk. A scripted run has no project directory, so only the model half answers
there — the disk half is for a project saved and reopened, which no headless
case reaches.

**`SObject::refCount()` IS NOT "HAS PLACEMENTS", and the obvious reading of it
is wrong.** The project's own `linkToFile` bookkeeping holds a reference, so an
extern file whose every clip has been undone away still reports a positive
count. Measured: a recording undone left the count at 1 and the first draft of
the scan reported nothing at all. "Placed" is answered by WALKING the
arrangement and collecting every object a link points at plus every window's
content — which also finds a take three levels inside a wrapped column.

A registered ASSET is exempt whatever the walk finds: an asset body is
something the user named and can place later, so "nothing places it right now"
is not a reason to offer to bin it.

**READ-ONLY, and that is what lets it be shared.** The Unused recordings dock
renders it, `cleanup-takes files="1"` acts on it, and
`assert-unused-recordings` asserts it — one function, so the display and the
sweep cannot disagree about which files are orphans.
