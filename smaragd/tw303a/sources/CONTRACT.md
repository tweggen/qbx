# tw/sources — CONTRACT

Purpose: sample data and everything that reads it — resident WAV material,
independent read cursors, rate views, loop windows, grain time-stretch.

Public headers: twrandomsource.h (THE data contract), twsamplesource.h,
twsamplereader.h, twresampledsource.h, twresampler.h, twcapturingsource.h,
twloopreader.h, twgrainsource.h, twgrainparams.h, twwavinput.h, twwav.h.

Depends on: tw/core, tw/pages, tw/graph. Forbidden: mix/playback/render
(sources do not know who consumes them).

The twRandomSource contract:
- read(srcOffset, dest, n, ch) is STATELESS, lock-free over immutable
  resident data, zero-fills past the end, returns frames actually read.
- viewAtRate(rate) is the ONLY rate-conversion seam (cached per rate);
  everything above it speaks project-rate frames.
- acquireReader(env, initialOffset) mints an INDEPENDENT cursor — cuts must
  never share twWavInput's single cursor (proposal 07).

Invariants:
1. twSampleReader::seekTo is ABSOLUTE in the source domain; the acquire-time
   offset is an initial position, not a base (MapPosFn adds slip offsets —
   POSITION_DOMAINS.md rule 3).
2. twLoopReader is CUT-RELATIVE: loop base baked in at construction.
3. twGrainSource runs in the STRETCHED domain; offsets scale by stretch.
4. The WAV loader clamps to bytes actually present (short-read clamp) — the
   header's frame count is not trusted; it warns and sizes to real data.

Threading: sources are immutable after load; readers are single-consumer
cursors (one per clip placement).

How to test: full qxa suite; grain_*.qxa for the grain path;
render_split_slip_offset.qxa for reader/offset semantics.

Known debt: loader supports 16-bit PCM only and scans the first 8 KiB for
the data chunk (naive RIFF walk); QString file paths (QtCore); linear
resampler is pitch-correct but not mastering-grade.
