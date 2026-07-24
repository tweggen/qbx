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

twGrainSource backend (proposal 26): the time-stretch / pitch-shift core is
Rubber Band Library (R3 engine), run in OFFLINE mode — the whole warp is
materialised once in the ctor, read() stays a memcpy. Selected by the
TW_HAVE_RUBBERBAND compile define (link discovered in tw303a/CMakeLists.txt);
when absent the file falls back to the legacy overlap-add. Rubber Band is fed in
BOUNDED blocks (kBlock=4096) with the output drained after each block: a single
whole-clip process() overflows its output ring and drops samples on any stretch
whose output exceeds ~262144 frames (noisy stderr + a wrong/missing warp).
setMaxProcessSize / setDebugLevel(0) complete that (pre-sized buffers, no library
stderr). The output is clamped/zero-padded to the EXACT nFrames_ =
floor(inLen*stretch) (invariant 3 below). NO output gain is applied: Rubber Band
is loudness-preserving, so a stretch/pitch keeps the source RMS (an earlier
peak-scaling anti-clip was removed — one Gibbs transient dimmed the whole clip).
Rubber Band is GPL, so the whole app is GPL. On x64-windows it is built with
Rubber Band's builtin FFT via the repo's vcpkg overlay port
(smaragd/vcpkg-overlays/rubberband) — the upstream port's sleef FFT fails under
MinGW.

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

How to test: `ctest -R sources_test` (reader absolute seeks, loop window,
zero-fill, grain stretch — sources/tests/); grain_*.qxa for the audible
grain path; qxa.render_split_slip_offset for offset semantics end-to-end.

Known debt: loader supports 16-bit PCM only and scans the first 8 KiB for
the data chunk (naive RIFF walk); QString file paths (QtCore); linear
resampler is pitch-correct but not mastering-grade.
