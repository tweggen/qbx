# app/model — CONTRACT

Purpose: the document model core. SObject (the tree node: properties,
ordered SLink children, aspect page cache, IRevalidatable implementation),
SLink (placement: parent + startTime), SProject (root, sample rate,
settings, revalidator), extern-file bookkeeping, and the SObjectRenderer
interface views implement.

Public headers: app/model/{sobject,slink,sproject,sprojectprops,
ssortedobjlist,sexternfile,sexternfilelist,sobjectrenderer}.h

Depends on (engine): tw/core, tw/graph, tw/pages, tw/schedule, tw/sources.
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

How to test: full qxa suite; action_roundtrip_test for serialization
adjacency.

Known debt: none of the former model→objects edges remain; the module is
ready to become a real build target once its remaining consumers are.
