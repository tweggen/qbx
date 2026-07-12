# app/objects/cut — CONTRACT

Purpose: the clip window object. SCut (startOffset/duration/loopLength/grain
window over any content, reader-chain ownership, container capture,
snapshot-based audio access), its inline renderer, and the window actions:
split-clip, unsplit-clip, resize-clip, duplicate-clip.

Public headers: app/objects/cut/*.h

Depends on (engine): tw/core, tw/graph, tw/pages, tw/schedule, tw/sources.
App edges: per tools/check_layering.py.

Invariants (normative detail: CLIP_MODEL.md, POSITION_DOMAINS.md):
1. Audio threads read via getSnapshot() only; window params change under
   mutex() then invalidate (drag path queues events, applies after).
2. mapTimelineToComponentPos mirrors seekTo exactly: ensureReader() first;
   identity for looping; +startOffset (×stretch under grain) otherwise.
   If you change one, change both.
3. rebuildReader is chain-descriptor-checked: plain trim/slip REUSES the
   reader; grain/loop changes mint a new one (fresh page cache).
4. startOffset_/cutDuration_ live in the grain OUTPUT (stretched) domain;
   split arithmetic relies on the second cut inheriting grain params RAW
   (setGrainParams would double-apply the rescale).
5. Container-backed cuts capture via freezePage of the content root;
   arrangementChanged drops the capture transparently.

How to test: render_split_slip_offset.qxa (THE regression),
render_sawtooth_clipped_section.qxa, grain_*.qxa.

Known debt: loop tiling of container captures deferred; FIXME bounds check
in seekTo.
