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
casts; lane-ness is isPathContainer, the active lane is activeLane, and
volumeDbSnapshot gives renderers a thread-safe volume read).
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
   still has no channel state. The SINK is still mono until B5, so a width
   change is inaudible in a rendered FILE — visible in the pages.

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
