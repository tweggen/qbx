# Concept: Sample Source / Reader Split + Universal Random Access

Design only. A foundational engine refactor that proposal 06 (Grain Playback)
depends on but which stands on its own. It removes the shared-cursor / shared-file
race documented across `twWavInput`, `SPlainWave`, and `SCut`, and — equally
important — makes time-stretching applicable to **anything** placed before an
`SCut`, not just file samples.

## TL;DR

- `twWavInput` today conflates three roles in one object that `SPlainWave`
  **shares** among all its cuts: (1) file ownership/decoding, (2) the RAM sample
  cache, (3) a playback **cursor** (`playOffset_`). Sharing role 3 is the bug;
  sharing roles 1–2 under a lock is the UI/audio race. (§0)
- **Split data from cursor.** A positionless, immutable **`twRandomSource`** owns
  the samples and offers *stateless random-access reads*; a tiny per-consumer
  **`twSampleReader`** holds the cursor. A source becomes a **factory of
  readers** — every consumer gets its own cursor over shared, read-only data.
  Cut-vs-cut stomping and cache thrash vanish by construction; full residency
  kills the file mutex. (§1, §3)
- **Universal random access (the wish).** `twRandomSource` is an *interface* with
  two implementors: `twSampleSource` (file-backed, naturally random) and
  **`twCapturingSource`** (wraps *any* linear `twComponent` — a synth, a sub-mix,
  another cut — and materializes its output into a buffer, presenting a
  random-access face). So a grain/time-stretch node can sit before an `SCut`
  whose content is *anything*. (§2)
- The grain node (06) reads `twRandomSource::read()` directly; plain consumers use
  `acquireReader()`. Both faces, one shared backing. Identity/passthrough still
  spins up none of this. (§4)

---

## 0. The root cause (what we're actually fixing)

`twWavInput` (see `tw303a/include/twwavinput.h`) holds, in one shared instance:

| role | fields | should be |
|------|--------|-----------|
| file owner / decoder | `file_`, `dataStart_`, `nSamples_`, `orgRate_/Bits/Channels_` | shared, immutable after load |
| RAM cache | `cache_`, `cacheStart_`, `maxCacheSize_`, `cacheSize_` | shared, immutable (or internally synchronized) |
| **playback cursor** | `playOffset_` | **per consumer** |

`SPlainWave::getRootComponent()` returns the one `cpWave_` to *every* consumer,
and `SCut::getRootComponent()` delegates straight to it. Consequences:

- **Cut-vs-cut interference.** `twTrackMix::calcOutputTo` pulls each clip via
  `lk->seekTo(...)` then `lk->getRootComponent().calcOutputTo(...)`. Two cuts of
  the same source alternate-seek the *same* `playOffset_` and blow the *same*
  cache window every block — correctness is saved only by the strict serial pull
  order, at the cost of constant cache refills.
- **UI/audio data race.** `getPreview()` (UI thread) and `calcOutputTo()` (audio
  thread) touch `file_`/`cache_` concurrently; today guarded by `fileMutex_`
  (a lock in the realtime path) and explicitly flagged as racy in the headers.

The band-aid in 06 (grain node's private cache + seek-before-refill) only narrows
the window and does nothing for the UI race. The real fix removes the shared
**mutable** state: data becomes immutable+shared, cursors become private.

---

## 1. The abstraction: `twRandomSource` + `twSampleReader`

```cpp
// Positionless, read-only, random-access view over sample data.
// Reads do NOT mutate any shared cursor → safe for many concurrent callers.
class twRandomSource {
public:
    virtual ~twRandomSource();

    // Stateless random read of one channel. Returns frames produced
    // (short only at end-of-material). No internal position is advanced.
    virtual length_t read( offset_t srcOffset, sample_t *dest,
                           length_t len, idx_t channel ) const = 0;

    virtual length_t length()    const = 0;   // frames, or -1 if unbounded/live
    virtual idx_t    channels()  const = 0;
    virtual twFormat format()    const = 0;   // reuses proposal-04 twFormat

    // Capability hints that drive caching/sharing (see §6):
    virtual bool isReproducible() const = 0;  // same read() ⇒ same bytes, forever
    virtual bool isBounded()      const { return length() >= 0; }

    // The "reader factory": mint an independent cursor over this data.
    twSampleReader *acquireReader();
};

// Thin per-consumer twComponent: just a cursor + back-pointer.
class twSampleReader : public twComponent {
    twRandomSource &src_;
    offset_t        pos_ = 0;
    // calcOutputTo(d,len,ch) == src_.read(pos_, d, len, ch); pos_ += produced
    // seekTo/tellPos/isSeekable trivially on pos_
};
```

**Naming (not final).** The working names are `twRandomSource` (interface),
`twSampleSource` (file impl, §2), `twCapturingSource` (adapter impl, §2), and
`twSampleReader` (cursor). The interface/`twSampleSource` pairing reads a little
close — they differ only by "Random" vs "Sample" — so an implementer should feel
free to rename for clarity (e.g. interface `twSampleAccess` with impls
`twResidentSource` / `twCapturingSource`, or similar). The *roles* are what
matter: one stateless random-access interface, two providers, one cursor type.

Key inversion vs today: **leaf sources are multiplicable; graph nodes are
singletons.** The old shared-`getRootComponent()` API is precisely what forced
the conflation. `twRandomSource` exposes *two faces* over one shared backing:

- **linear consumers** (a plain cut, `twTrackMix`) call `acquireReader()` → an
  owned cursor, fully independent of every other reader;
- **random consumers** (the grain node, §4) call `read()` directly, since grain
  access is inherently non-monotonic (overlap, lookback, reorder).

---

## 2. Two providers — and why that satisfies "time-stretch anything"

`twRandomSource` is an interface with two concrete implementors:

### `twSampleSource` — file / resident (the common case)
Owns the decoded data + RAM residency (absorbs `twWavInput`'s cache role).
`read()` is a bounds-checked memcpy/convert out of resident data — naturally
random, `isReproducible() == true`, `isBounded() == true`. If fully resident
(a 6-min 48 kHz stereo file ≈ 66 MB — load it; fall back to a windowed/streaming
mode only above a size threshold), reads are **lock-free**: many readers, zero
writers after load. That alone dissolves the UI/audio race.

### `twCapturingSource` — wraps *any* `twComponent` (the wish)
This is what lets you "put just about anything before an `SCut`." It adapts a
generative/streaming source — a synth (`twSimpleSaw`), a sub-mix
(`twTrackMix`), another cut, even another grain node — into a random-access face:

- Holds the wrapped component (a `twSampleReader` it acquired, or a raw
  `twComponent*`) and a **materialization buffer**.
- `read(off..off+len)`: if covered, serve from the buffer; if past the
  high-water mark, pull the source **linearly** (`calcOutputTo`) forward,
  appending, until covered; if `off` precedes retained history and the source is
  seekable+reproducible, re-seek and re-pull, otherwise serve from retained
  history only.
- **Bounded vs unbounded.** A bounded reproducible source (offline synth render)
  materializes once into a growable buffer → behaves like a file. A live/endless
  source uses a **bounded ring** of recent history: random access only within the
  window — which is exactly what grain overlap/lookback needs, and all an endless
  stream can honestly offer.

So the grain/time-stretch node (06) never special-cases its input: it always
sees a `twRandomSource`. The `SCut` seam decides which provider to interpose
(§4). Time-stretch becomes a uniform capability over the whole object graph.

---

## 3. Thread-safety / residency

- Fully-resident `twSampleSource`: immutable after load ⇒ concurrent UI + audio
  reads need **no lock**. Delete `fileMutex_` from the realtime path.
- Windowed/streaming fallback (huge files): the shared window is mutable, so it
  needs a non-blocking scheme (double-buffer / seqlock, or a per-reader small
  cache over a rarely-locked backing fetch). Prefer residency; treat streaming as
  the exception, not the default.
- `twCapturingSource`'s buffer is written by whoever drives materialization. Keep
  materialization on the consuming (audio) thread to avoid cross-thread writes,
  or guard with a single-producer ring. (Detail for the impl phase.)

---

## 4. Threading it through the object model

- `SPlainWave` owns a `twSampleSource` (replaces `cpWave_`).
- **Contract change:** the source-side API shifts from "get *the* node" to
  "**acquire** a node I own." Add `SObject::acquireReader()` (or, more precisely,
  `acquireRandomSource()`/`acquireReader()` pair). `getRootComponent()` stays for
  genuine singletons (`twTrackMix`, mixers).
- The `SCut` seam (06 §3) resolves its content to a `twRandomSource`:
  - content **is** a sample source → use its `twSampleSource` directly (shared,
    reproducible — full cross-cut caching, 06 §7);
  - content is **anything else** → wrap `content.getRootComponent()` (or an
    acquired reader) in a `twCapturingSource`. Shareable across cuts iff the
    wrapped source `isReproducible()` + seekable; otherwise per-stream.
  - Then: feed that `twRandomSource` to the cut's `twGrainPlayer`.
- **Plain (non-stretched) cut:** acquires a `twSampleReader` and returns it from
  `getRootComponent()` — independent cursor, the cut-vs-cut fix, *no grain node,
  no capture buffer*. Identity stays free (06 §3).

Net: every `SCut` ends up as `[twRandomSource over content] → (optional)
twGrainPlayer → output`, with time-stretch applying uniformly regardless of what
the content is.

---

## 5. Lifetime & ownership

- `twSampleSource`: owned by `SPlainWave`, lives with the source object.
- `twSampleReader` / `twCapturingSource`: owned by the **consumer** that acquired
  them (the `SCut`, or transiently the grain node); destroyed with it.
- `SObject`s are already reference-counted and shared via `SLink`, so the source
  cannot be freed while a consumer still holds a reader — make `acquireReader()`
  take/hold a reference, release on reader destruction.
- Register components with `tw303aEnvironment` following the existing pattern so
  teardown is uniform.

---

## 6. Caching & sharing — how capabilities gate it (ties to 06 §7)

The `isReproducible()`/`isBounded()` hints decide how far the 06 §7 tiers reach:

| source kind | tier-1 (samples) | tier-2 (analysis) | tier-3 (rendered grains) |
|-------------|------------------|-------------------|--------------------------|
| file `twSampleSource` | shared, resident | per-source, persistable | full content-addressed share |
| bounded **reproducible** captured (offline synth) | materialize once, shareable | per-(source+params) | shareable |
| live / non-reproducible captured | per-stream ring | n/a (no stable identity) | per-stream only |

So the cache key in 06 §7 must incorporate the source's identity *and* its
reproducibility: a content hash for files, a parameter hash for reproducible
generators, and "uncacheable / per-stream" for live material. Sharing degrades
gracefully along this axis rather than being all-or-nothing.

---

## 7. Migration — each step independently shippable

1. **Extract `read(offset, …)`** inside `twWavInput` from its existing cache
   logic; reimplement `calcOutputTo` as `read(playOffset_)`. Pure no-op refactor.
2. **Promote** that to a standalone `twSampleSource`; leave `twWavInput` as a thin
   `twSampleReader` wrapper for back-compat. `SPlainWave` owns the source, still
   hands out one reader → behaviour identical to today.
3. **`acquireReader()` contract:** switch `twTrackMix`/`SCut` to acquire their own
   readers. Independent cursors land here — interference & thrash gone.
4. **Full residency** for `twSampleSource` → drop `fileMutex_`, killing the
   UI/audio race.
5. **`twCapturingSource`:** add the universal adapter; let the `SCut` seam wrap
   non-sample content. "Time-stretch anything" lands here.
6. **Grain node** (06) consumes `twRandomSource::read()` directly; wire 06 §7
   caches keyed by the §6 capability table.

Steps 1–4 are pure win independent of grain work (they fix a real race and a real
CPU waste). 5–6 are the bridge into proposal 06.

---

## 8. Relationship to proposal 06

- 06 §2/§7's one honest caveat ("per-cut nodes still pull the single shared
  `twWavInput`, so two cuts grain-reading one source still race on its cursor") is
  **dissolved** here: stateless `read()` over immutable data has no shared cursor.
- This also **unlocks** the source-level grain mode 06 §2 ruled out: once a source
  is a reader/sample factory, a *source* could mint grain-readers, each with its
  own state — the obstacle ("a shared node can't serve independent positions") is
  exactly what the factory removes. Not in scope, but no longer blocked.
