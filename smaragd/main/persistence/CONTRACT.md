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
   attributes (serialization_roundtrip_test guards the Fraction layer).

How to test: load-project + save-project qxa actions; the test4 user
project is the realistic corpus.

Known debt: an unknown element name in a project file warns and yields a
null link (unchanged legacy behavior) — consider a hard load error.
