# tw/sources — CONTRACT

Purpose: sample data and everything that reads it — resident sample material
(WAV plus MP3/FLAC/AIFF/Ogg/Opus via libsndfile), independent read cursors,
rate views, loop windows, grain time-stretch.

Public headers: twrandomsource.h (THE data contract), twsamplesource.h,
twsamplereader.h, twresampledsource.h, twresampler.h, twcapturingsource.h,
twgrowingcapturesource.h, twloopreader.h, twgrainsource.h, twgrainparams.h,
twwavinput.h, twwav.h.

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
8. **PAGE WIDTH IS THE SOURCE'S CHANNEL COUNT** (proposal 36 B3).
   `twSampleReader`, `twLoopReader` and `twWavInput` return it from
   `getOutputChannels()`, so `twComponent::freezePage` allocates their pages
   that wide. These are the FIRST production components in the tree that are
   ever wider than one channel; everything else still declares the default 1.
   A mono source declares 1 and takes byte-for-byte the pre-B3 path.
9. **A wide render is ONE seek, ONE pass, ONE cursor advance** (§4.3). Each of
   the three fills every channel of the page from the SAME position and
   advances its cursor once (`twWavInput` does not advance at all — its
   historic contract is that callers seek before every block). A per-channel
   loop over `calcOutputTo()` would advance a whole page per channel and fill
   channel 1 with the NEXT page's audio; `wide_reader_test` compares every
   page channel against `twRandomSource::read()` at the same position, which
   is the only assertion shape that can see that failure. **A subclass that
   changes WHERE frames come from must override `renderPageWide()` too** —
   `twLoopReader` does, and inheriting the linear version would silently turn
   a looping stereo clip into a single linear pass.
10. **`renderFrames()` stays un-overridden on purpose** (§7 trap 18). Base
   `renderFrames()` calls `calcOutputTo()` and base `calcOutputTo()` calls
   `renderFrames()`; a component overriding NEITHER recurses until the stack
   ends. These classes override `calcOutputTo(IOVector&)`, so a mono scratch
   page still renders channel 0 through the narrow path.
11. `getNOutputs()` and `getOutputChannels()` agree in these three classes and
   ONLY here — a file cursor's ports really are its channels. They must not be
   merged (§7 trap 8): `twRewire`'s ports are buses. `twWavInput::getNOutputs()`
   returned a hardcoded 4 with one latch built until B3; it now builds one
   latch per channel, which is what gives §4.4 rule (1)'s plug clamp something
   to select.
12. **A CAPTURE IS AS WIDE AS THE THING IT CAPTURED** (proposal 36 B7).
   `twCapturingSource` has taken a `channels` parameter since proposal 07 and
   every caller passed 1, so its planar `channels * nFrames` arithmetic had
   never been executed above width 1. `SCut::buildCapture_` now passes the
   container's declared width (or, for the grained preview capture, the grain
   source's) and a `twSampleReader` over the capture is wide by invariant 8 —
   which is the whole of how a container/asset clip keeps its channels. NOTE
   the buffer is the ONE allocation in the engine that multiplies by channel
   width, and it is per placement rather than shared (proposal 06 §7's
   content-addressed capture cache does not exist yet): it is accounted by
   `PageAccounting::onCaptureAllocated` and printed as `captureBuffers=` by
   `<report-page-memory>`. SIZED AT B9, because "multiplies by width" understates
   it — the multiplier is (width x rebuilds x placements) and only the first
   factor came from proposal 36. Measured on the corpus: `captureBuffers`
   230 400 B at width 1 against 1 843 200 B at width 8 (exactly x8), while
   `buildCapture_` runs **7-9 times in a single run** for a corpus with one
   asset clip, because `invalidateCapture()` has ten call sites and two
   independent rebuilders (the UI thread through `rebuildReader`, a revalidator
   worker through `revalPrepPreview`) race for the same work with no
   memoization, and `oldReader_` holds a `captureRef` until `~SCut`. That, not
   the page model, is where a width-8 memory problem would actually come from
   (proposal 36 §7 trap 25). B9 measured it and did not fix it: the fix is the
   content-addressed cache, which is proposal 06 §7's scope.
   **The live-component capture constructor is GONE** (trap 27, deleted at B9):
   its per-channel `seekTo` + `calcOutputTo` loop was exactly the cursor-
   displacing shape §4.3 forbids, and it had never had a caller. Do not confuse it with `CapturePagePool`, which is
   an unrelated pool of an unrelated type and does NOT widen (pages/CONTRACT.md).

13. **A GROWING CAPTURE IS THE ONE SOURCE WHOSE LENGTH CHANGES** (proposal 21
   L3a, design D7). `twGrowingCaptureSource` is the recording counterpart of
   invariant 12's snapshot: chunked planar storage, one chunk per
   `chunkFrames` frames of every channel (default `twOutputPage::FRAME_CAPACITY`,
   static_asserted), an atomic `frontier()` that only grows, and a width fixed
   at construction. Four things a change here must preserve:
   (a) **ONE producer, any number of readers.** `append`/`appendPlanar`/
   `reserveThrough` are single-writer; `read`/`readInterleaved`/`frontier` are
   callable from anywhere, including the RT-adjacent pump.
   (b) **The frontier's release store is the ONLY publication.** Samples and
   the chunk pointer that holds them are both written before it, and a reader
   only touches frames below the frontier it acquired, so nothing else needs
   ordering. A reader must never trust a chunk pointer it found above the
   frontier.
   (c) **A read past the frontier is a SHORT READ, never a wait.** Live
   material has no "not yet" answer to give, and a reader that blocked on one
   could deadlock the audio thread. `isReproducible()` is therefore **false**
   (the same read can return more next time), so no derived-data cache may key
   on it.
   (d) **The chunk INDEX never reallocates.** It is a fixed array of atomic
   pointers sized at construction (default 4096 chunks — 1.55 h at 48 kHz);
   appending past it is REFUSED and counted in `droppedFrames()`. A
   `std::vector` that reallocated would move the samples a reader is copying
   out, which is exactly why the storage is chunked at all.
   `toCapturingSource()` is the handover to invariant 12's fixed source and
   costs exactly ONE copy of the audio — the flat planar buffer
   `twCapturingSource` adopts is built straight out of the chunks and moved in.
   Accounted through the same `PageAccounting::onCaptureAllocated` as
   invariant 12, per chunk.

Threading: sources are immutable after load; readers are single-consumer
cursors (one per clip placement). The streaming grain's block LRU is the one
mutable, mutex-guarded exception (invariant 6). `twGrowingCaptureSource` is the
second: it is mutable BY DESIGN, lock-free, and safe only under invariant 13's
single-producer rule.

How to test: `ctest -R sources_test` (reader absolute seeks, loop window,
zero-fill, grain stretch, and the width-4 capture round trip of invariant 12 —
sources/tests/); `qxa.mc_capture_clip_width` (invariant 12 end to end: a
stretched, a pitched and a container/asset clip over `tests/test_stereo.wav`,
each asserted at the clip AND in the rendered file); `ctest -R wide_reader_test`
(invariants 8-11 over the committed `tests/test_stereo.wav`, plus the
`warp.pcm` channel-count key check of proposal 36 AC B3.4); `ctest -R
record_bridge_test` (invariant 13 — odd-sized appends across chunk boundaries,
the short read at the frontier, the masked interleaved read, the index-exhausted
refusal and the one-copy handover); grain_*.qxa for the audible
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
