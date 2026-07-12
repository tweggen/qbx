# app/persistence — CONTRACT

Purpose: project file I/O. SProjectLoader (DOM two-pass load: instantiate
via object dictionary, then wire links/attributes) and the save/load
actions. Wire format background: plan/proposed/04_WIRE_FORMAT_AND_SAMPLE_RATE.md.

Public headers: app/persistence/*.h

Depends on (engine): tw/core, tw/graph (via model). App edges: knows every
object type (instantiateFromDomElement dispatch) — the biggest single knot
in the app SCC.

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

Known debt: type dispatch is hardcoded per class — a registration-based
factory removes persistence→objects edges (Phase 6).
