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
   Writers produce files atomically (write to a PRIVATE temp
   "<path>.<pid>.<seq>.tmp", fsync, rename over the target); any validation
   failure on open (magic, format version, header CRC, ascending region bounds
   vs. file size) makes the file count as ABSENT.
   The temp name is unique per WRITER, not per key, and that is load-bearing:
   one store root is shared by every process on the machine, so several
   processes routinely stage the same key at once (`ctest -j` runs the whole
   suite against one store, and every case using the same fixture derives the
   same content hash → the same key). A shared "<path>.tmp" would let two
   writers truncate and interleave into one file, and only the header carries a
   CRC — a torn payload of the right length passes the region-bounds check and
   would be read back as valid analysis data, breaking invariant 4. Eviction
   only ever considers `*.qaf`, so temps are never evicted; a temp leaked by a
   crash between open and rename is collected by deleting the store (inv. 1).
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
- The aspect registry (twaspects.h) now carries the shipped aspects:
  preview.peaks v1, onsets v2 (NORMALIZED spectral flux — v1's absolute flux
  fired spurious onsets on steady loud material; v1 files orphan on sight),
  loudness v1, and warp.pcm v2 (params blob gained an onsetsHash so warps
  built before/after the onsets sidecar occupy different keys). The `stft.if`
  spectral aspect proposed for the vocoder was DROPPED by design: the paged
  vocoder's analysis is incremental (a lazy windowed FFT over resident PCM
  beats reading persisted spectra), so it needs no analysis sidecar.
- The variable-stride seek-table path is unimplemented: writers always emit
  seekTableOffset = 0 and no aspect uses a variable stride yet.
