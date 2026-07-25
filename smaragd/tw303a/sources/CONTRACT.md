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

twGrainSource backend (proposal 27 M5): the DEFAULT time-stretch / pitch-shift
core is the in-house `twPagedVocoder` (tw/sources/twpagedvocoder.h) run in
STREAMING mode — no materialised warp at all. The ctor takes a `sharedRef()`
co-owning the source PCM and borrows its planar channels; read() renders aligned
output blocks (kBlock=65536) on demand through the vocoder, cached in a small
mutex-guarded LRU (see StreamState). Memory is O(concurrent blocks), not
O(clip × variants) — the Reaper-scale property proposal 27 exists for. The
backend is RUNTIME-selected by stretchBackend() (TW_STRETCH_BACKEND, read once
per process): `vocoder` (default), `rubberband`, or `ola`.

Rubber Band (proposal 26) is now the OPTIONAL reference / escape-hatch backend
(TW_STRETCH_BACKEND=rubberband), still gated by the TW_HAVE_RUBBERBAND compile
define (link discovered in tw303a/CMakeLists.txt). Its path MATERIALISES the
whole warp once in the ctor (read() a memcpy). It is fed in BOUNDED blocks
(kBlock=4096) with the output drained after each block: a single whole-clip
process() overflows its output ring and drops samples on any stretch whose output
exceeds ~262144 frames (noisy stderr + a wrong/missing warp). setMaxProcessSize /
setDebugLevel(0) complete that (pre-sized buffers, no library stderr). NO output
gain is applied: Rubber Band is loudness-preserving, so a stretch/pitch keeps the
source RMS (an earlier peak-scaling anti-clip was removed — one Gibbs transient
dimmed the whole clip). On x64-windows it is built with Rubber Band's builtin FFT
via the repo's vcpkg overlay port (smaragd/vcpkg-overlays/rubberband) — the
upstream port's sleef FFT fails under MinGW. Rubber Band is GPL, so while it is
linked the whole app is GPL; since M5 it is no longer load-bearing (dropping it
is a build-config decision, not a capability loss). `ola` is the legacy
overlap-add, the fallback when Rubber Band is absent.

Every backend clamps/zero-pads output to the EXACT nFrames_ =
floor(inLen*stretch) (invariant 3 below). The proposal-27 M2 `warp.pcm` sidecar
still serves the materialise paths (RB/ola/offline-vocoder) as a durable byte
cache; the streaming default writes no `warp.pcm` — pages render on demand.

Invariants:
1. twSampleReader::seekTo is ABSOLUTE in the source domain; the acquire-time
   offset is an initial position, not a base (MapPosFn adds slip offsets —
   POSITION_DOMAINS.md rule 3).
2. twLoopReader is CUT-RELATIVE: loop base baked in at construction.
3. twGrainSource runs in the STRETCHED domain; offsets scale by stretch.
4. The WAV loader clamps to bytes actually present (short-read clamp) — the
   header's frame count is not trusted; it warns and sizes to real data.
5. Streaming twGrainSource LIFETIME: the StreamState holds a `sharedRef()`
   co-owning the source PCM for as long as the grain exists — clip-content
   teardown during a queued freeze cannot dangle the borrowed channel views
   (the materialise paths were safe by copying; streaming must own instead).
6. Streaming read() may COMPUTE: a page fault renders one or more vocoder
   blocks synchronously. It therefore runs on freeze/worker threads only and
   is NEVER reached from the RT audio thread (RT reads frozen pages —
   proposal 16/19). The block LRU is mutex-guarded (workers race shared grains).
7. twPagedVocoder is bit-exact under partition: rendering an output range in
   any set of consecutive windows (any worker, any order) equals rendering it
   in one call — the vocoder_test property gate. This is what makes the
   streaming grain safe under the page scheduler.

Threading: sources are immutable after load; readers are single-consumer
cursors (one per clip placement). The streaming grain's block LRU is the one
mutable, mutex-guarded exception (invariant 6).

How to test: `ctest -R sources_test` (reader absolute seeks, loop window,
zero-fill, grain stretch — sources/tests/); grain_*.qxa for the audible
grain path; qxa.render_split_slip_offset for offset semantics end-to-end.

Known debt: loader supports 16-bit PCM only and scans the first 8 KiB for
the data chunk (naive RIFF walk); QString file paths (QtCore); linear
resampler is pitch-correct but not mastering-grade.
