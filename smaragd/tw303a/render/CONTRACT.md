# tw/render — CONTRACT

Purpose: THE rendering engine — RenderSession renders a time range of a
component graph to an audio file on a worker thread, via sequential
freezePage (no seekTo state corruption) and FileSink.

Public headers: render_session.h.

Depends on: tw/core, tw/pages, tw/graph, tw/sinks, tw/playback (AudioEngine
types). Forbidden: app headers — positions/progress flow OUT through
onPosition/onProgress/onComplete callbacks only.

Invariants:
1. Page positions are ABSOLUTE timeline: currentPos = startOffsetSamples_ +
   samplesWritten (a marked range does not start at 0) —
   POSITION_DOMAINS.md rule 6.
2. Pages are requested page-aligned; each page passes as previousPage of the
   next so DSP state chains (FREEZE_PROTOCOL.md); the first page resets.
3. Callbacks run ON the render thread: handlers must be realtime-safe and
   Qt-free (THREADING.md rule 1).
4. start() rejects overlapping renders; requestCancel() is safe from any
   thread; the file is closed even on cancel/error.

How to test: every render_*.qxa and grain_*.qxa case; the region-RMS
assertions in render_split_slip_offset.qxa are the strongest gate.

Known debt: output is duplicated-mono stereo (proper multi-channel TBD);
Extent enum in RenderParams is advisory (start/end seconds are what counts).
