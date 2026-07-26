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

How to test: load-project + save-project qxa actions; the test4 user
project is the realistic corpus.

Known debt: an unknown element name in a project file warns and yields a
null link (unchanged legacy behavior) — consider a hard load error.
