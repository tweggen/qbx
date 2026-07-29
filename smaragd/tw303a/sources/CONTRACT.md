# tw/sources — CONTRACT

Purpose: sample data and everything that reads it — resident sample material
(WAV plus MP3/FLAC/AIFF/Ogg/Opus via libsndfile), independent read cursors,
rate views, loop windows, grain time-stretch.

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
per process): `vocoder` (default) or `ola`.

Rubber Band (proposal 26) was REMOVED 2026-07-26 on the requester's decision:
the in-house vocoder had been load-bearing since M5, and dropping the
GPL-licensed dependency exercises the relicensing freedom proposal 26
recorded. The warp.pcm params-blob backend byte 1 stays RESERVED for the
retired path (never reuse it — historical cache keys must not alias).
`ola` is the legacy fixed-hop overlap-add (proposal 06), kept as a
dependency-free reference; audible warble on tonal material, not a quality
path.

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
   header's frame count is not trusted; it warns and sizes to real data. The
   libsndfile path (twSampleSource::loadSndfile, used for every non-16-bit-WAV
   format) applies the same clamp on a short decode, and produces the byte-
   identical planar-Float32 layout + content hash the WAV fast path does.
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
grain path; qxa.render_split_slip_offset for offset semantics end-to-end;
qxa.mp3_sample_import for the libsndfile decode path (RMS discriminator over a
committed MP3 fixture — never a byte-cmp, since mpg123 decode is not
reproducible across versions/platforms).

Known debt: the hand-rolled fast path is 16-bit-PCM-WAV only and scans the
first 8 KiB for the data chunk (naive RIFF walk) — everything else, including
non-16-bit WAV, falls to libsndfile. MP3 read needs libsndfile built with
mpg123 (vcpkg `mpeg` feature / Homebrew mpg123 dep); MP3 carries a small
decoder delay. QString file paths (QtCore); linear resampler is pitch-correct
but not mastering-grade.
