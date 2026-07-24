# tw/sidecar — CONTRACT

Purpose: the derived-data substrate (proposal 27). A self-describing,
versioned, CRC-protected container (QAF) plus a size-capped, LRU-evicted
directory store keyed by (content hash × aspect id × params hash). Everything
it holds is DERIVED — a precomputed analysis of source PCM (e.g. waveform
preview peaks) that can be regenerated at will.

Public headers: twqaf.h (container: `twCrc32`, `twQafWriter`, `twQafReader`),
twsidecarstore.h (`twSidecarStore`), twaspects.h (the closed aspect-id/version
registry — constants only).

Depends on: tw/core (twcontenthash, twlog). Forbidden: everything else. No Qt,
no engine graph state — std::filesystem and cstdio only.

Threading: a `twQafReader`/`twQafWriter` instance is single-threaded; distinct
instances on distinct files are freely concurrent. `twSidecarStore` serializes
every method under one mutex; callers may use it from any non-RT thread.

Invariants:
1. Sidecars are derived. Deletion is always safe; absence of a file simply
   means the consumer recomputes and (optionally) re-stores. The store is a
   cache, never a source of truth.
2. A failed or partial file is never observable and is never "repaired".
   Writers produce files atomically (write to "<path>.tmp", fsync, rename over
   the target); any validation failure on open (magic, format version, header
   CRC, ascending region bounds vs. file size) makes the file count as ABSENT.
3. Identity match is total: aspect id + aspect version + content hash + params
   hash must all agree for a load hit. A file whose aspectVersion no longer
   matches is orphaned — deleted on sight (it can never become valid again).
4. The store disabled (unset root) is behaviorally identical to the store
   enabled, minus speed: load() misses, store() no-ops, the engine result is
   unchanged. Sidecars must never alter output, only latency.
5. Never called from the RT audio thread — it does file I/O. Callers are
   background/aspect and UI threads only.

How to test: round-trip a `twQafInfo` + payload through `twQafWriter::write`
and `twQafReader::open`/`readRecords`/`readAllPayload`; assert header fields
survive and that byte corruption of the header fails open(). Store tests use a
temp-dir root: verify keying/pathFor spelling, load-miss on version bump (with
on-sight deletion), mtime touch on hit, and that `evictIfNeeded` drops
oldest-mtime `*.qaf` files past the cap while skipping locked ones.

Known debt:
- The aspect registry (twaspects.h) is constants-only until M1; there is no
  generator registration yet (that arrives with the background-job work).
- The variable-stride seek-table path is unimplemented: writers always emit
  seekTableOffset = 0 and no aspect uses a variable stride yet.
