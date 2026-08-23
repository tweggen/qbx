# app/model — CONTRACT

Purpose: the document model core. SObject (the tree node: properties,
ordered SLink children, aspect page cache, IRevalidatable implementation),
SLink (placement: parent + startTime), SProject (root, sample rate,
channel count, settings, revalidator), extern-file bookkeeping, and the SObjectRenderer
interface views implement.

Public headers: app/model/{sobject,slink,sproject,sprojectprops,
ssortedobjlist,sexternfile,sexternfilelist,sfilepathref,sobjectrenderer,
sclipwindow}.h

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
muted body and the waveform colour are HSL moves off that one anchor
(`body()` / `wave()`), never separate constants. So the picker this is built for
— any colour, later — gets its whole family for free, and there is no way for
one mount to invent a "bright" variant another mount disagrees with.

| Rule | Why |
|---|---|
| **`SObject::colorIndex_`, -1 = AUTO**, resolved to the lane's position in the flattened lane order | On `SObject` rather than on `STrack` for the reason `contentKind()` and `resolveEventClip()` are: the serializer, a future picker action and the pixel gates must reach it without knowing which slice owns the lane. |
| It is written to the file **only when it is not -1** | Every project saved before the palette — and every committed golden — serializes byte-unchanged. The same discipline as `editGroup`, `midiRouting` and `recordingChannels`. |
| The resolved pair travels on **`SRenderContext::clipColors()`** | A wave, a cut and an event clip all draw in their track's colour while none of them may include `app/objects/track`. Whoever fills the body sets it; every nested `InlineRenderContext` inherits its parent's. |
| The auto index is resolved **ONCE PER LANE**, never per clip | It walks the project's lanes. Per clip that is O(lanes x clips) on every repaint. |
| **THE PIXEL GATES CALL THE SAME FUNCTIONS THE PAINTER DOES** | `assert-lane-overlay` and `assert-take-lane` classify a grabbed lane by exact colour and by luminance band, and they used to hardcode `QColor(160,160,160)` as "the clip body" — true only while every clip in every project was one grey. `smainwindow.cpp`'s `sClipBodyOf()` is the shared spelling, exactly as proposal 41 M7 gave paint and hit-test one `tagChipRect()`. |
| The anchors are **mid-tone and quiet** (S 20-44 %, L 36-48 %), ordered so NEIGHBOURS DIFFER IN HUE | The selected variant is the same hue made lighter, so an anchor near white would have nowhere to go. And the auto assignment is by lane order: a hue RAMP would make two lanes stacked on top of each other nearly identical. |

**A luminance BAND is no longer a safe way to describe a third colour.** The
"overlay" band in `assert-lane-overlay` is `lumFill < lum < lumClip`, and the
clip body dropped from 160 to ~102 when it became a track colour — so the
feel-flow heatmap's own LUT, which used to sit inside that band, now sits ABOVE
it (measured: 8854 pixels in `lighterThanClip`, 380 left in the band). That
gate's load-bearing assertions are exact-colour LUT membership and are immune;
its informational `minPixels` was retired rather than retuned. Anything new
that wants to be found by colour should be found by IDENTITY.
