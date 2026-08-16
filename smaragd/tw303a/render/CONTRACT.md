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
5. An INVERTED range is rejected; an EMPTY one (end == start) is not. A
   zero-length render opens the writer, writes its header and closes with 0
   frames — a valid, well-formed, empty file. That is the honest render of an
   empty arrangement, and it is why the caller cannot mistake "nothing to
   render" for "the render failed" (it used to get neither a file nor an
   error, because start()'s failure is not propagated through
   SAppContext::startRender).
6. The frame count is ROUNDED from the extent, not truncated: the extent is a
   frame count divided by the sample rate, and a ratio that is not exactly
   representable would otherwise land one frame short of the arrangement.

How to test: `ctest -R render_test` (absolute-range content, onPosition,
page-boundary continuity against a scripted component — render/tests/);
every render_*.qxa and grain_*.qxa case end-to-end.

7. The output file's CHANNEL COUNT is the graph's, not a constant
   (proposal 36 B5). `RenderParams::channels` is 0 by default, meaning "ask
   `synthOutput->getOutputChannels()`", and the root's declared width IS
   `SProject::channels()` — so a channels='6' project renders a 6-channel file
   and a channels='1' project a mono one. A caller may pin a number; the
   render then warns if it disagrees with the graph, clamps a channel the page
   does not have to the page's last (the §4.4 read clamp) and drops the rest.
   It used to be `config.channels = 2` with the single mono page written into
   both channels ("Duplicate to stereo (temporary; proper multi-channel TBD)").
8. Frames reach the sink as INTERLEAVED BLOCKS, one call per render block —
   `AudioSink::writeFrames(interleaved, nFrames, channels)`. The old
   frame-at-a-time `writeFrame(AudioFrame&)` is retired along with `AudioFrame`
   itself, whose `MAX_CHANNELS == 2` was the hard stereo cap.

Known debt: Extent enum in RenderParams is advisory (start/end seconds are what
counts).
