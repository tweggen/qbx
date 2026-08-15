# app/persistence — CONTRACT

Purpose: project file I/O. SProjectLoader (DOM two-pass load: instantiate
via object dictionary, then wire links/attributes) and the save/load
actions. Wire format background: plan/proposed/04_WIRE_FORMAT_AND_SAMPLE_RATE.md.

Public headers: app/persistence/*.h

Depends on (engine): tw/core, tw/graph (via model). App edges: {actions,
model} — shell-free since Phase 6 (sloadprojectaction lives here now, and
the loader's stale sapplication include is gone). Since Phase 5 the loader names NO concrete types: each
object slice self-registers its element name via
SProjectLoader::registerSObjectClass() from a static initializer (the app
must stay an OBJECT library or those TUs are dropped at link).

Invariants:
1. Loading runs with invalidation SUPPRESSED (disableInvalidation /
   enableInvalidation around createObjects) — revalidation storms during
   deserialization deadlocked historically.
2. Object identity in files is the id attribute; links reference objectId
   through the loader dictionary; order of definition matters (content
   before referencing cuts).
3. Positions serialize as Fractions (exact); durations in frames at the
   PROJECT's sampleRate attribute; legacy files default 44.1 kHz.
4. Loaded projects must re-serialize byte-equivalently modulo volatile
   attributes (serialization_roundtrip_test guards the Fraction layer and the
   base64 state-chunk layer; plugin_slot_roundtrip.qxa /
   plugin_missing_placeholder.qxa guard a whole plugin slot, state chunk
   included).
5. A reference carried by a plain ATTRIBUTE is resolved by
   deferResolve(), never inline (proposal 08 M4). The instantiation loop only
   defers an element until each of its <SLink objectId> CHILDREN is in the
   dictionary; an attribute reference (STrack's pluginChainId) is invisible to
   that ordering, so reading one during construction may find nothing.
   deferResolve() queues a resolver that createObjects() runs after
   setRootComponent and before ~SProjectLoader — dictionary complete, handle
   links still alive. Resolvers run once, in order, on the loading thread;
   anything a resolver keeps must take its own reference, because those handle
   links are deleted immediately afterwards.

5b. BOTH ACTIONS SET SProject::setProjectFilePath(). External file
   references are stored relative to the project file (SFilePathRef —
   app/model/sfilepathref.h), so the project has to know where it is on
   disk: SSaveProjectAction sets the anchor after the target file opens and
   before serialize(); SLoadProjectAction sets it before createObjects(), so
   the first <SPlainWave filename='...'> already has something to resolve
   against. The anchor is NOT the serialized fileName attribute and is never
   read out of the document — a path baked into the file describes where the
   project USED to be, which is precisely the case relative storage exists to
   survive.

6. ONE BAD ELEMENT MUST NEVER COST THE WHOLE PROJECT, and the instantiation
   loop must always terminate. Two failure modes, both hit by a real user file:
   (a) a child with no id= was answered with "File is corrupt" and a return
   -1, which discarded EVERYTHING. Nothing can reference an object with no id
   (references are <SLink objectId>), so such an element is now skipped, and
   it costs exactly itself. This mattered because every build before proposal
   08 M4 wrote <SPluginSlot> without an id, making its own files unopenable.
   (b) The loop rescans the document until it has no children left, so an
   element whose <SLink objectId> names an object the file does not contain
   can never be consumed and spun forever — measured at ~3M passes/minute with
   an unbounded log and RSS. Every pass now tracks whether it consumed
   anything; a pass that consumes nothing names each unresolvable element and
   its dangling target, drops them, and ends the loop. (c) instantiate()
   returning NULL — commonest cause an <SPlainWave> whose sample is missing
   (after basename recovery next to the project has already been tried,
   splainwave.cpp) — was ALSO a return -1; it is now skipped like (a), and the
   (b) leftover sweep then cascades the drop to any SLink/SCut that referenced
   it, so one lost sample no longer loses the project. All recoveries WARN
   with the type and identity of what was lost — silent data loss would be
   worse than the abort they replace. Gate: sample_missing_survives.qxa.
7. A FAILED load must not resume background work on the graph it abandons.
   createObjects() returning non-zero leaves a half-built object graph that
   the caller discards (SMainWindow::openProjectFile marks it partial and
   deleteLater()s it). SLoadProjectAction therefore calls pauseRevalidation()
   — which blocks until in-flight worker jobs drain — BEFORE balancing the
   invalidation counter, and ~SProject's isPartialLoad_ early return does the
   same before handing the children to ~QObject. Without it the worker pool
   held raw pointers into objects that were being freed; the symptom was a
   crash on the SECOND open attempt, the first attempt's last log line being a
   preview recompute.

6b. THE RECOVERY IS PER ELEMENT KIND, AND IT ITERATES (proposal 36 D8a).
   Invariant 6(b)'s single sweep dropped every leftover at once, which
   cascaded: a missing sample killed its `<SCut>`, the cut killed the
   `<STrack>` that placed it, the track killed the `<SStdMixer>`, and the load
   ended with "root component not found" — one lost file, no project. The
   sweep now applies a policy by element kind and RETRIES the instantiation
   loop until it reaches a fixed point:

   - **Container** (`STrack`, `SStdMixer`, `STakeStack`) — drop the dangling
     `<SLink>` and keep the element. A track that lost one clip is still the
     user's track, and every other clip on it is still their work.
   - **Window** (`SCut`; `SMidiCut` from P1) — drop the ELEMENT. A window
     whose content is gone has nothing to show; its own placement becomes a
     dangling link on the next pass, where the container rule handles it.
   - **Plain** (everything else, and every unregistered/unknown element) —
     drop the element, exactly as before.

   The kind is declared at registration (`registerSObjectClass(..., kind)`),
   so persistence still names no concrete type; unregistered means Plain, so
   an element from a newer build costs itself and nothing else. Each pass
   removes at least one node or link, and a pass that repairs NOTHING falls
   back to dropping every leftover — termination is not left to the policy.
   Every repair still WARNS with the type, the id and the missing target.

   THE ROOT IS THE ONE THING THAT MAY NOT BE RECOVERED: `createObjects()`
   returns -1 when `rootId` does not resolve. Everything the document
   describes hangs off it, so an empty shell that reports success would look
   like an opened project and overwrite the file on the next save. Gates:
   `load_unknown_object_survives.qxa`, `load_missing_sample_placed_survives.qxa`.

7b. `<SProject formatVersion='N'>` (proposal 36 D8a). Written unconditionally
   (`SProject::FORMAT_VERSION`, 2 today); read with a default of **1**, which
   is what every pre-proposal-36 file is. A HIGHER version is warned about and
   then read anyway — a reader that refused would strand a user's file on
   whichever build they happen to have, while an element it does not know is
   already skipped by name with its own warning. Nothing branches on the
   version yet; it exists so that something can.

How to test: load-project + save-project qxa actions; the test4 user
project is the realistic corpus. legacy_project_recovery.qxa carries both
of invariant 6's defects in one fixture (an id-less <SPluginSlot> and a
chain linking to an id that exists nowhere) and asserts the rest of the
project survives intact; it has a CTest TIMEOUT because a regression of 6(b)
HANGS rather than fails.

Known debt: an unknown element name in a project file warns, yields a null
link and is skipped; since 6b the links that referenced it are pruned rather
than cascading, so it costs exactly that object (`load_unknown_object_survives`
is the gate). A hard load error would be wrong for exactly the same reason
7b gives.
Pre-M4 projects cannot have their plugin PLACEMENT recovered: the track ->
chain reference was not serialized either, so a recovered slot would rejoin
a chain no track owns. Such slots are dropped and must be re-added by hand.
