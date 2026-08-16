# Proposal 36 — Configurable multichannel signal flow (mono / stereo / 4 / 6 / 8)

> **Status: DRAFT v3 (2026-08-15).** v3 applies an adversarial review of v2 that
> found three blockers: the channel-at-the-plug mechanism did not exist and was
> never designed (§4.6); the default per-channel render loop would corrupt every
> cursor-bearing component, starting with the one it was meant to serve (§4.3);
> and B4/B6 were internally contradictory for any track with a plugin (now one
> milestone). It also corrected a factual error in §2 that changed the milestone
> ordering. The review's findings are recorded inline rather than summarised, so
> the reasoning survives.
>
> **v2 (2026-08-15).** v1 recommended extending the existing
> "N channels = N parallel mono component instances" model and holding the page
> rewrite behind a measurement gate. **The requester chose the page rewrite
> instead**, so v2 adopts it as the design and re-plans everything downstream of
> it. v1's cost analysis is kept in §3 unchanged — it is the reason the choice is
> a real one, and it is the list of things that must not be rediscovered.
>
> This proposal answers a gap `plan/STATE.md:7741` recorded and left unowned:
> *"The multi-channel sink above is nobody's milestone yet and **should become
> one**."*
>
> Prerequisite reading: `docs/contracts/FREEZE_PROTOCOL.md` §"Page geometry"
> (the contract this proposal amends), `plan/proposed/19_ASYNC_FREEZE_MODEL.md`
> ("Phase 2 REVISED"), `plan/proposed/04_WIRE_FORMAT_AND_SAMPLE_RATE.md`
> (`twFormat` already carries `channels`/`layout`),
> `plan/proposed/08_PLUGIN_HOSTING.md` §"Design premise 2" and §"Settled
> decisions 5, 6" (the decision this proposal reverses, and the channel-mismatch
> policy it keeps), `smaragd/tw303a/plugins/CONTRACT.md:364-372`,
> `plan/todo/COMPONENT_CHAIN_BUGS.md` BUG 3.

---

## 1. Was there already a plan? No.

A full sweep of `plan/` and `docs/` found **no proposal, design note or backlog
item** for channel width. What exists is the same limitation recorded in nine
places, plus two decisions that deliberately scoped it out while leaving hooks:

| Where | What it says | Kind |
|---|---|---|
| `plan/STATE.md:7687-7693, 7741, 7883, 8049` | "the sink is still mono… should become [a milestone]" — restated three times | gap, unowned |
| `plan/todo/08_PLUGIN_HOSTING_EXECUTION.md:12-14` | the mono sink "is a tw_render / tw_playback gap… and is nobody's milestone" | carry-over |
| `plan/proposed/04…:54-56, 608-610` | "`channels`/`layout` exist in the descriptor but no component produces > 1 channel yet" | decision: deferred |
| `plan/proposed/08…:87-93, 422-442` | channels are parallel mono wires; ">2 channels… until a routing matrix exists" loads bypassed | decision: architecture |
| `plan/proposed/34…:77-79, 183-185` | "Never write an `L != R` assertion"; stereo meters gated on this work | gap |
| `plan/todo/COMPONENT_CHAIN_BUGS.md:60-75` | BUG 3, right channel always silent; "Full stereo requires the mixer's bus count to match" | bug + gap |
| `plan/todo/NESTED_LANE_STALE_PAGE.md:152` | a fixed bug described as "a trap primed for the stereo-output work" | latent |
| `docs/contracts/FREEZE_PROTOCOL.md:49` | pages are 65536 **mono** frames, normatively | decision |
| `plan/todo/BACKLOG.md:24` | "multi-channel capture (mono for now)" | backlog, one line |

`plan/proposed/29_FOLLOWUPS.md` — the "what's next" routing document — does not
mention channel width at all.

---

## 2. What is actually true today (and it is not "the engine is mono")

The received summary is *"the engine is mono"*. That is wrong in a way that
changes the plan:

**Already N-channel, working, tested:**

- **Every source.** `twSampleSource` decodes to planar Float32
  (`channel c, frame f at data_[c*nFrames_ + f]`), and so do `twResampledSource`,
  `twGrainSource` and `twPagedVocoder` (whose `Config.channels` is exercised at 2
  in its own tests).
- **The reader already treats `idx` as a channel.**
  `twSampleReader::getNOutputs()` returns `src_.channels()` and builds one latch
  per channel; `calcOutputTo(dest, idx)` calls `src_.read(pos_, …, idx)`.
- **A track really is two buses**: two `twTrackMix` + two `twPluginChain` joined
  by a 2-plug `twRewire`; gain fans out; plugin taps render both.
- **The file writers, the WASAPI backend, the whole analysis layer and
  `dump-playback-capture` are genuinely N-channel** and merely pinned to 2.

**But the audio never reaches bus 1 through the freeze path.** v2 claimed it did;
that was wrong, and the correction reorders the plan. Every bus's `twTrackMix`
receives the *same* clip with the *same* `getComponentFn`/`resolveFn`
(`strack.cpp:252-265`); the resolved component's page cache is keyed by position
alone (`outputPages_`, `twcomponent.h:594`) and filled at `idx = 0`
(`twcomponent.cc:848`). **Bus 0 and bus 1 therefore receive the identical
channel-0 page, for every clip.** The per-channel latches of `twSampleReader` are
only reachable through the legacy `calcOutputTo` pull; the freeze path never uses
them. So the narrowing happens in four places, earliest first:

1. **The clip / reader seam — for ALL clips.** The resolved component freezes one
   mono page at `idx = 0`, so a stereo file's channel 1 is computed by nobody.
   This is the broadest destruction point and v2 missed it entirely.
2. **The clip capture**, `SCut::buildCapture_`: reads channel `0` into a
   `twCapturingSource(…, 1, …)`. ~~so stretched, pitched and container-backed
   clips are mono a second time over~~ — **only half true, corrected by B3**:
   `rebuildReader` calls `buildCapture_` *only when there is no random source*,
   i.e. for container/asset-backed clips. A **sample-backed** stretched or
   pitched clip builds `twGrainSource` directly over the source and reads it
   through a plain `twSampleReader`, with no capture on the playback path at
   all; `buildCapture_`'s grained branch serves **preview**. So sample-backed
   stretch and pitch went wide in B3, not B7, and B7's scope is narrower than
   §5 first said: container/asset clips and the preview capture.
3. **The master**, `SStdMixer::setNBusses(1)` (ctor, hard-coded, never
   serialized): the summing loop runs `bus < 1`, so every track's bus 1 is built,
   filtered, plugin-processed — and dropped. `getRootComponent()` still carries
   `// FIXME: Generate a channel reassignment.`
4. **The sink**, twice: `RenderSession` (`config.channels = 2;` +
   `bufR[i] = sample;  // Duplicate to stereo (temporary; proper multi-channel
   TBD)`) and `AudioEngine::pullBlock(outL, outR, n)`, on top of
   `AudioFrame::MAX_CHANNELS = 2`; then `twSpeaker` fans out `(c % 2 == 0)`.

The one place two genuinely different buses exist today is *downstream of a
stereo plugin*, because `twPluginSlotProcessor` renders all buses itself and the
`twtestclap` skew fixture manufactures the cross-channel term. That is the
existing coverage, and it is why it was built that way.

Plus: **width cannot change at runtime.** `STrack::setNBusses` grows only —
shrink is `Q_ASSERT_X(false, …)` — and the loader default (`nBusses="1"`)
contradicts the ctor's 2.

---

## 3. The fork, and why B was chosen

Proposal 08 settled it once: **N channels = N parallel mono wires = N parallel
component instances**, because the frozen page is mono by contract:

```
twOutputPage:  std::vector<float> samples;   // FRAME_CAPACITY "mono frames",
                                             // no channel field, no stride, no layout
freezePage_nolock: renderFrames(page->samples.data(), …, 0 /* idx = 0 */)
page cache:    std::map<offset_t, page>                    // (component, position)
inflight:      std::map<offset_t, InFlightFreeze>          // (component, position)
scheduler:     using NodeKey = std::pair<const twComponent*, uint64_t>;
plan dep:      struct twPageDep { producer; pageStart; }   // no output index
bound inputs:  struct Entry { producer; pageStart; page; } // no output index
```

**The costs of extending that model** (v1's analysis, unchanged — these are the
reasons B was chosen, and they are why the work below is worth its risk):

- `twPluginSlotProcessor` exists *only* to work around it. Its header: *"It is
  deliberately NOT a twComponent… so N parallel mono wires are N parallel
  component instances"*; `twplugininsert.h`: *"a component that wrote interleaved
  stereo into one page produced garbage the engine then read as mono."* Every
  channel-coherent stage needs that out-of-band processor + per-bus tap + private
  cache.
- Per-bus instantiation has already produced bugs: the `setNBusses` grow crash
  and shrink assert; the pre-M3 `twPluginChain` loop that fed a 2-in plugin *"bus
  audio on input 0 and SILENCE on input 1"*; the nested-lane duplicate-key trap
  logged as *"primed for the stereo-output work"*.
- It multiplies the DAG: 8 channels = 8× nodes, 8× dependency counting, 8× epoch
  bookkeeping, 8× invalidation walks, 8× components per track.

**What B costs, stated honestly, because it is what this plan must survive:** it
changes the most load-bearing contract in the engine and touches every
`freezePage` override, the page pool, the latch seam, capture, sidecar keys,
metering and preview. Byte-exactness is verifiable only after the fact. §5 is
ordered specifically to make that survivable: **the entire mechanical sweep
happens while every page in the system is still one channel wide**, so the
byte-`cmp` gate is green by construction through the riskiest phase.

**What B buys, concretely:**

| | today (A) | under B |
|---|---|---|
| scheduler key | `(component, position)` | **unchanged** |
| 8-channel DAG | 8× nodes | 1× nodes |
| channel-coherent DSP | processor + N taps + private cache | ordinary component |
| `twSampleReader` | N latches, only latch 0 ever frozen | one wide page, dead code retired |
| planar source → page | narrowed to channel 0 at the reader | flows straight through |
| a track | N × (`twTrackMix` + `twPluginChain` + inserts) | one of each, width N |

---

## 4. The design

### 4.1 The page carries its channels, planar, with a constant stride

```cpp
struct twOutputPage : PageBase {
    static constexpr size_t FRAME_CAPACITY = 65536;   // per channel, unchanged
    std::uint16_t channels = 1;                       // NEW
    std::vector<float> samples;                       // channels * FRAME_CAPACITY

    float       *channelPtr( idx_t c );               // &samples[c * FRAME_CAPACITY]
    const float *channelPtr( idx_t c ) const;
};
```

**The stride is `FRAME_CAPACITY`, not `validFrames`.** A stride that tracked the
valid length would change as tails shorten, and every consumer would have to
learn it at read time; a constant stride is a compile-time fact. Planar, not
interleaved, because *every source in the engine is already planar* and because a
mono page must stay byte-identical to today's.

~~`getDataPtr()` keeps returning channel 0, so every existing call site remains
correct for width 1~~ — **there are no such call sites: `getDataPtr()` has zero
callers in the entire tree** (found by B1b; only the two overrides exist, in
`twOutputPage` and `CapturePageData`). It is dead polymorphic API and a
width-blind hole; B9 should delete it alongside `twFormatCaps::channelCounts`.
Every *real* consumer was reviewed and converted in B1, because "correct today,
silently wrong at width 2" is the bug class this proposal exists to eliminate —
and B1b made the page's buffer **private**, which turns that from a grep result
into something the compiler enforces.

**`sampleCount() == channels * FRAME_CAPACITY` is NOT universally true**, and v3
did not know it. A pre-existing **mono scratch** path resizes a page's buffer to
an arbitrary caller-chosen length — `twComponent::calcOutputTo` (both overloads)
and `IOVector::CreateFromBuffer` build a throwaway page as a plain buffer. B1b
preserved it as `resizeMonoScratch()` (width-1 only; refuses a wide page).
**`channelFrames()` is therefore the only honest frame bound**, in every phase
from here on.

**Memory:** page bytes = `channels × 256 KiB`. A mono project's footprint is
unchanged to the byte. 8 channels × 4 readahead pages × 30 tracks ≈ 240 MiB —
the same total the parallel-wire model would have used, in one allocation instead
of eight.

**That estimate was the wrong order of magnitude, and B1a measured why.** The
corpus resides at **49 pages / 12.8 MB** of `twOutputPage` after a render — but
`SProject` eagerly builds a `CapturePagePool` of **2048 pages = 528 MiB per
project** (`sproject.cpp:551`), of which the corpus uses *one*. So `twOutputPage`
is not the dominant term today and a page-count figure alone understates the
process by ~40×. Two consequences this proposal must carry: **(a)** `releaseOldPages`
**has no caller anywhere in the tree**, so component page caches are pruned only
by invalidation and teardown and `outputPages_` grows unbounded across a session
— at width 8 each retained page is 2 MiB, so what is untidy today is a real
problem at width 8; **(b)** `CapturePageData` (the pool's element, and the
preview/capture page type) is a *different* type from `twOutputPage`, and B7 must
decide explicitly whether it widens too — an eager 528 MiB pool multiplied by
width is not something to discover late.

### 4.2 A component declares its width

```cpp
virtual idx_t getOutputChannels() const { return 1; }
```

Deliberately **not** `getNOutputs()`. That is the patch-bay port count, and the
two mean different things in different classes today — `twRewire`'s N plugs are
buses, `twSampleReader`'s N outputs are channels, `twWavInput` returns a
hardcoded 4 with one latch created. Conflating them is how this stays broken.
Default 1 ⇒ every existing component is unchanged and correct.

### 4.3 Rendering: width 1 keeps today's path; width > 1 MUST render wide

v2 proposed a default per-channel loop over `renderFrames(…, idx)`. **That is
wrong and must not be built.** `twSampleReader::calcOutputTo` — the very
component the loop was meant to serve — advances a cursor (`pos_ +=
dest.length()`, `twsamplereader.cc:58-72`), while `freezePage_nolock` seeks
*once* before rendering (`twcomponent.cc:825-826`). A loop would render channel 0,
advance the cursor a whole page, and fill channel 1 with the **next page's
audio** — the "coherent page displaced by one page" bug this repo has already
bled for. The same applies to every input-side plug cursor, and `internalState`
is captured once after rendering (`twcomponent.cc:851-855`), so a state chain
would be meaningful for channel 0 only.

The rule instead:

```cpp
// width 1  -> byte-for-byte today's code path. Not "equivalent to": the same code.
// width > 1 -> the component MUST override renderPageWide(); the base
//              implementation for width > 1 asserts and logs. It never
//              silently renders something plausible.
// SKETCH — not what shipped; see below.
virtual bool renderPageWide( twOutputPage &out, const twFrozenInputs &in, … );
```

**That signature is not buildable** (B2). `freezePage_nolock` has no
`twFrozenInputs` in hand — the set is *thread-scoped*
(`twFrozenInputScope::active()`) — and threading it in explicitly would change
the width-**1** path's signature, which is the one thing that must not move. What
shipped mirrors `renderFrames` (`twOutputPage&`, frames, input, inputLength) and
a wide component reaches for bound pages via `twFrozenInputScope::active()`
itself, which is what §4.4 rule 2 already implies.

**The fork is on `page->channels()`, not on the declaration** — so width 1 makes
byte-for-byte the same `renderFrames(channelPtr(0), …)` call as before.

A wide component seeks once, fills every channel of its page in that one pass,
and advances its cursor once — which is what `twSampleReader` wants anyway
(`src_.read(pos_, out.channelPtr(c), len, c)` per channel, then one `pos_ +=`).
No implicit per-channel input mapping exists, so none can be got wrong.

### 4.4 Two rules for width adaptation, both stated per PAGE

> **(1) A plug pull yields channel `min(latchIndex, page->channels - 1)` of the
> page the producer actually froze.**
> **(2) A wide component reads its bound input PAGES directly and picks channels
> itself.**

Rule (1) is the mechanism v2 assumed and never specified. It lives in
`twStreamingLatch::copyData`, which today is channel-blind (`memcpy(pDest +
written, page->samples.data() + inPage, …)`, `twstreaminglatch.cc:208`) and
already carries the index it needs (`twLatch(component0, idx0)`, ctor line 13-14)
without ever consulting it. Giving that stored index its channel meaning is the
whole change, and it makes `twSampleReader`'s existing per-channel latches
correct rather than dead. Rule (2) is why no channel argument has to be threaded
through `readStreamingData`/`copyData` for wide consumers: they already receive
whole pages at the two seams (`twcomponent.cc:514-523` and the latch's bound
path).

**B4 correction: rule 2 needs a SHARED implementation, not a second one.**
Reading "a wide component reads its bound pages directly" as independent of
`copyData` would duplicate the staleness rule — and two copies of that rule is
how the "held page stamped with one epoch against a chain at another" bug got
written here once already. B4 extracted a single `acquirePage()` that both seams
call.

The clamp reproduces today's behaviour exactly — it is what
`twSampleSource::read` already does (`if( ch >= channels_ ) ch = channels_ - 1;
// mono plays on every channel`). Downmix policy (average vs. select) belongs to
whatever component wants it, **never** to the plug.

**Both rules say `page->channels`, never "the producer's declared width."** The
width you may act on is the width of the page in your hand. A component's
declared width is a promise about *future* pages, and the tree already launders
pages between components: an insert-less `twPluginChain` forwards its
`twTrackMix` page verbatim (`twpluginchain.cc:243-249`) and its silence pages are
default-constructed width 1 (`:224, 250, 268, 276`).

### 4.5 Width changes, stale pages, and the RT thread

`twOutputPage::channels` is **immutable after allocation**. A project width change
bumps the global content epoch, so cached pages of the old width are stale by the
existing mechanism rather than a new one.

Stale-page fallback (proposal 16) deliberately serves stale pages during live
playback, so it is the one path where a page of the *wrong* width could reach the
RT callback (`audio_engine.cc:310` reads `&currentFrozenPage_->samples[…]`) or
`twLevelProbe` (`tw_level_probe.cc:112`). Reading `channelPtr(1)` of a stale
width-1 page is an **out-of-bounds read on the audio thread**. Therefore:

> **A stale page whose `channels` differs from its PRODUCER'S DECLARED WIDTH is
> treated as a MISS, never as audio.** *(v3 said "the width the consumer
> expects"; B2 corrected it. "Consumer expects" is undefined without new
> plumbing, and worse: a narrow consumer of a correctly-wide page would read as a
> mismatch — which would silence playback for the entire gap between B4, when the
> graph goes wide, and B5, when the sink does. Comparing `page->channels()` to
> `producer->getOutputChannels()` rejects only pages that predate a width change;
> fresh pages pass by construction. This does not weaken §4.4's "act on the page
> in your hand", which answers a different question: **which channel to read**.)*
> Playback falls back to silence for that
> page; a meter decays. Width mismatch is the one staleness that is not
> tolerable.

### 4.6 What does NOT change

The scheduler, the demand system, invalidation, content epochs and
metering-by-position all key on `(component, position)` and stay exactly as they
are. **No bus dimension is added anywhere.** That is the whole point of B.

`IOVector` — the mixing seam `twTrackMix` actually uses (`mixFrom`,
`twtrackmix.cc:536-539`) — **stays mono by design**: it becomes a view over one
`channelPtr(c)`, and wide mixing is a loop over channels of the same page pair.
Deciding this here rather than inside B1's sweep is deliberate; it is exactly the
kind of choice that otherwise gets made silently and differently in six places.

*B1b built it mono against a single named `kChannel` constant, deliberately
without a channel selector — an unused parameter in the one phase whose entire
safety argument is that nothing is untested would have been self-defeating.*
**B4 must add that selector** the moment a wide `twTrackMix` mixes channel *c*.
Recorded in `pages/CONTRACT.md`.

---

## 5. Milestones and acceptance criteria

Standing gate for **every** milestone — written once, assumed in each:

```
./build.sh                                   # the re-configure is load-bearing (CONFIGURE_DEPENDS glob)
python tools/check_layering.py
python tools/check_logging.py
ctest --test-dir smaragd/build --output-on-failure     # reconcile registered vs run vs skipped
```
plus the **byte-exactness gate**, which needs a named corpus or it is a wish:

> Two fixture projects live in `smaragd/tests/goldens/`: `mc_mono.qxp`
> (`channels=1`) and `mc_stereo.qxp` (`channels=2`). Each is ~4 s and must
> contain, at minimum: a plain clip, a stretched clip, a pitched clip, a
> container/asset clip, a nested track, and a track with a `twtestclap` insert —
> i.e. one of every path this proposal touches. Their renders are frozen as
> committed 16-bit PCM WAVs at the **start of B1** and `cmp`'d at every
> milestone thereafter. Re-freezing requires an AC that licenses it and a written
> explanation of why the bytes moved.
>
> **The WAVs are committed to the repo** (requester decision, 2026-08-15). This
> is new for this project — exactness has so far been `cmp`'d across runs and
> builds, never against a stored file — and it is what makes "byte-identical to
> the pre-milestone golden" enforceable across milestones, machines and weeks
> rather than aspirational. Keep them short: they are a gate, not a demo.

A case that fails once and passes on re-run is not a pass: pin with
`repeat_test.sh` over `SMARAGD_REVAL_WORKERS` {1,4,8,16} and report either way.

### M0 — Make the gates able to see channels ✅ **EXECUTED 2026-08-15** (`50f1e29`)

> Delivered: the whole-file/region split collapsed (a whole file *is* a region
> with `frameCount < 0`), `assert-channels-differ` measuring both level
> (`minRmsDelta`) and content (`minDiffRms`, which catches equal-level-but-
> different channels), a committed 4-channel fixture with an exact RMS ladder
> (0.5 / 0.25 / 0.125 / 0.0625) from a committed generator with a `--verify`
> mode, and fixture path resolution. `channel_assert_dupmono.qxa` asserts the
> duplicated-mono sink **via `expectReject`** — it is a gate that is *supposed to
> break at B5*, which is the cleanest possible signal that the sink went wide.
> See §7 trap 1 for what this proposal got wrong about it.

The suite currently cannot detect this feature landing or regressing.

1. `assert-audio-energy` / `assert-audio-peak` **silently ignore `channel=` when
   `frameCount="-1"`** (both take a separate whole-file path that hard-codes
   `channelIndex = -1`). Invisible today because the channels are equal; it would
   silently mis-pass the day the sink goes wide.
   *Executed 2026-08-15 — and three of this item's premises were wrong:*
   `assert-audio-frequency` and `assert-source-position` do **not** have the bug
   (`estimateFundamental` already handled `frameCount < 0` correctly and
   `decodePositionAt` requires a positive window), so it is two verbs, not four.
   The fold is **not** an "all-channel mean": it is the *pooled RMS* for energy
   and the *max over channels* for peak — which is why a dropped `channel=` on a
   peak assertion can only mis-report a quiet channel as loud, never the reverse,
   and why the fixture case bounds channel 1 with `maxPeak=0.40`. Two blocking
   holes went unmentioned in this proposal and had to be closed to deliver M0 at
   all: the assert verbs could resolve `filename` **only** against the test
   output dir, so "commit a fixture and assert on it" was not achievable; and an
   out-of-range `channel=` reported RMS 0 / peak 0 rather than failing, so a typo
   could masquerade as a silent render.
2. Add `assert-channels-differ` so "genuinely different audio" is assertable.
3. Commit a reproducibly-generated asymmetric 4-channel WAV fixture.

**AC 0.1** The fixed verbs FAIL with a wrong `channel=` and PASS with the right
one, with `frameCount` both given and omitted.
**AC 0.2** `assert-channels-differ` fails on a duplicated-mono render, passes on
the fixture.
**AC 0.3** Every existing `.qxa` case passes unchanged.

### M1 — Channel count becomes project data (inaudible) ✅ **EXECUTED 2026-08-15** (`10b58db`)

> Delivered as specified. Three of this milestone's own premises were wrong and
> are corrected in place below; see also §7 traps 9-11, which M1 found and which
> B4 in particular must not rediscover.

- `SProject::channels()` + `<SProject … channels='N'>`, read with the
  `sampleRate` **warn-and-default** idiom (missing ⇒ 2 + one warning).
  ~~propagated to `tw303aEnvironment` as `setSRate` is~~ — **struck: not
  implementable and self-contradictory.** `tw303aEnvironment` has no channel
  state at all (only `bufferSize`, `sampleRate`, `candidateRates_`), so
  "propagate" would have meant inventing engine state inside the milestone whose
  defining constraint is that it touches no engine state. The constraint won.
  Valid widths are 1/2/4/6/8, gated in one place; an unsupported value is
  *rejected* by the action and *warned-and-defaulted* on load, because a project
  must never fail to open over this.
- `set-project-channels` action; `docs/ACTIONS.md` updated (hand-maintained).
- Fix the `nBusses` loader/ctor drift (`"1"` default vs ctor 2).

**M1 is project DATA ONLY. Nothing propagates to any bus count until B4.**
`STrack::setNBusses` shrink is still `Q_ASSERT_X(false, …)` at this point
(`strack.cpp:337-341`), so an undo of 6→2 that reached it would assert. The
action must be inert with respect to the graph, and its AC must prove that.

**AC 1.1** Save→load→save byte-equivalent (`persistence/CONTRACT.md` inv. 4).
*Note for every later milestone that cites it:* `serialization_roundtrip_test` is
a `tw_core` test over `Fraction` and base64 — **it never sees a `.qxp`**. It is a
"don't break this" check, not evidence about document round-tripping. Name a real
document-level gate instead.
**AC 1.2** A legacy `.qxp` without `channels=` loads as 2 and warns once.
**AC 1.3** `channels='6'` survives a round trip on a project with nested tracks.
**AC 1.4** `set-project-channels` and its undo touch no `setNBusses` call
(assert by instrumentation or by inspection recorded in the PR), and a render
before/after the action is byte-identical.

### B1 — The page grows a channel dimension, still always 1 (**the big sweep**)

`channels` field, `channelPtr()`, and **every consumer of `page->samples`
converted to an explicit accessor**: the latch seam, `twTrackMix` (and its
`IOVector` use, per §4.6), `twPluginChain`, capture, `RenderSession`,
`AudioEngine`, `twLevelProbe`, preview (`readContainerFrames`), `getPagesInRange`
users. Nothing declares width > 1 yet.

**There is no `twOutputPage` pool to resize.** Pages are `make_shared` on demand
(`twcomponent.cc:377, 582`; `twtrackmix.cc:382`) into per-component maps. So this
phase **builds page-memory accounting** (resident pages and bytes, per component
and globally) rather than adjusting a pool that does not exist. `CapturePagePool`
is a *different type* (`CapturePageData`) — and v3's claim that it is "used in
production nowhere" was **wrong**: `SProject` builds one of 2048 pages, 528 MiB,
eagerly per project (`sproject.cpp:551`). See §4.1.

Fix in the same sweep, because widening multiplies it:
`releaseOldPages` compares `it->first + twOutputPage::PAGE_SIZE < keepAfterPos`
(`twcomponent.cc:393`) — **bytes (262144) against a frame position**, so retention
is already ~4× too generous. Hunt the whole frames-vs-bytes class here.

Pull the synthetic 4-channel component (v2 had it in B2) **forward into B1** as a
unit-level probe: it is the only thing that can catch a wrong conversion, since
both the grep and the width-1 byte gate pass a mistake happily.

**AC B1.1** Byte-exactness gate green against the frozen corpus — and it *means*
something here, because this phase is pure mechanism.
**AC B1.2** A grep-backed inventory in the PR body: every `samples`/`getDataPtr`
call site, converted or justified. No raw `samples.data()` outside the page class
and the wide-render path. (This AC is necessary and **not sufficient** — a wrong
conversion passes it; B1.4 is what actually detects one.)
**AC B1.3** Page-memory accounting exists, is reported for the corpus, and is
unchanged at width 1 versus a pre-B1 measurement of the same projects.
**AC B1.4** The synthetic 4-channel component's page round-trips through
`channelPtr()` with four distinguishable signals at unit level.
**AC B1.5** `releaseOldPages` retention is frames-vs-frames, with a test that
pins the boundary.

### B2 — Components declare width; the wide render path exists ✅ **EXECUTED 2026-08-15** (`316bf5d`)

`getOutputChannels()`; `renderPageWide()` with the §4.3 rule (width 1 keeps
today's code, width > 1 must override, base asserts); the §4.4 plug channel rule
in `twStreamingLatch::copyData`; the §4.5 width-mismatch-is-a-miss rule. The
synthetic 4-channel component from B1 is promoted from unit probe to a real
graph participant.

`getOutputChannels()` is **authoritative for page width from this milestone on.**
`twFormatCaps::channelCounts` is currently seeded `{1}` and never narrowed
(`twcomponent.cc:29-45`); it is either seeded from `getOutputChannels()` here or
deleted in B9 — it may not sit alongside as a second, drifting authority.

**AC B2.1** The synthetic component's page carries four distinguishable channels,
asserted at several positions, **through the real scheduler**.
**AC B2.2** The §4.4 clamp: a width-1 consumer of a width-4 producer reads
channel 0; a width-4 consumer of a width-1 producer reads that channel on all
four. Both asserted, both paths (plug pull and bound page).
**AC B2.3** A width > 1 component that does *not* override `renderPageWide`
asserts rather than rendering — proven by a test that expects the failure.
**AC B2.4** *(B2: "node count unchanged" can only ever be a **distribution**, not
a number — `nodesExecuted` jitters ±2 run-to-run at fixed workers on an unchanged
tree. `nodeRetries` and `missPages` are the stable, sharper observables. B9.2 must
plan for that.)* Byte-exactness gate green. Scheduler node count on the corpus
unchanged — noting that at B2 nothing in production is wide, so this AC shows
*"no regression"*, **not** B's central claim; that is B9.2's job.

### B3 — The clip path goes wide ✅ **EXECUTED 2026-08-16** (`5fc51e0`)

> Three components became wide and nothing else did: `twSampleReader`,
> `twLoopReader` and `twWavInput`. Three things this milestone taught:
> **(a)** `twLoopReader` needed its own `renderPageWide` — inheriting the linear
> one would have turned a looping stereo clip into a single linear pass, audible
> and *invisible to any width-1 gate*. **(b)** `twWavInput`'s hardcoded
> `getNOutputs() → 4` mattered beyond tidiness: `SCut::resolveClip` falls back to
> the CONTENT's root component until a reader exists, and that fallback was the
> last narrowing point. **(c)** Width needed **no** threading through `twView` /
> `twLoopMap` — it survives by construction — and B3.4 needed **no** version bump,
> because `warp.pcm` already carries the channel count as field 3 of its params
> blob and re-checks it before adopting. The gate forges two entries at the same
> key differing only in that field: the right-shaped one must be adopted, the
> wrong-shaped one must miss.

Per §2's correction, this is the broadest narrowing point and it must come before
the sink has anything true to say. `twSampleReader` renders all channels into one
page in one pass with a single cursor advance (§4.3), and its per-channel latches
become the plug mechanism of §4.4 rather than dead code; **the clip resolution
chain (`twView` / `twLoopMap` / `SCut`'s resolve) carries width through**;
`twWavInput`'s hardcoded `getNOutputs() → 4` inconsistency is resolved;
grain/vocoder `channels` are threaded through (both are already channel-aware).

Capture-backed clips (stretch / pitch / container) stay mono here — that is B7 —
so B3's ACs are scoped to **plain** clips and must say so.

**AC B3.1** A stereo WAV's page carries two distinct channels at the reader,
asserted at engine level.
**AC B3.2** A plain stereo clip's width **survives the resolution chain**: the
component `twView::resolve` returns for the clip yields a two-channel page with
distinct channels, asserted through the real scheduler — not merely at the
reader. *(v3 asserted this at the TRACK'S ROOT component, which is not
achievable in this milestone and would have forced whoever built it to either
drag B4's work forward or quietly weaken the AC: `twTrackMix` and
`twPluginChain` are width 1 until B4, so a two-channel page cannot exist there
yet. The track-root assertion moves to B4.6.)*
**AC B3.3** A mono WAV renders byte-identically; the corpus gate holds.
**AC B3.4** `grain_*`, `warp_*`, `exact_*` green; content/sidecar keys that must
encode channel count are bumped so no old entry is a wrong-shape hit (assert a
**miss**, not a wrong hit).

### B4 — The whole track path goes wide, plugins included ✅ **EXECUTED 2026-08-16**

> A track is now **one `twTrackMix` + one `twPluginChain` + one `twRewire`, N
> channels wide**, and a slot is **one `twPluginInsert`**. Gone with the per-bus
> instantiation: the `setNBusses` grow-crash, the shrink-assert, the processor's
> all-bus cache and the sideways sibling gather. `nBusses='2'` retires from the
> writer and is read-and-ignored — a track has no bus count, it has the
> project's width, and a derived per-track copy would be the second authority
> whose drift already cost this project once. `twPluginSlotProcessor` **stays**,
> as the plugin lifetime/state holder only: proposal 08 inv. 18 depends on a
> slot's graph identity *being* its processor, so a rescan hands it a new factory
> rather than re-wiring every chain.

v2 split this from the plugin work and the split was incoherent: the tap
architecture *requires* one chain per bus (`twpluginchain.cc:65-66, 161-212`;
`strack.cpp:381-414`), so a wide `twTrackMix` feeding per-bus chains has nowhere
to put the bus-1 tap and — by §4.4 — no way for it to read channel 1. Either the
chain widens with everything else, or an adapter gets built and thrown away. So
this milestone is one piece:

- `STrack` builds **one** `twTrackMix` + **one** `twPluginChain` of width N;
  per-bus instantiation retires, and with it the grow-crash and shrink-assert.
- `twPluginInsert` becomes a wide component (`renderPageWide`); the
  `twPluginSlotProcessor` all-bus cache and per-bus tap fan-out retire (the
  processor may remain as the plugin lifetime/state holder).
- Proposal 08's channel-mismatch policy (Direct / DualMono / MonoFold /
  Unsupported) is **preserved semantically** and re-derived from page width
  instead of bus count.
- `twRewire` becomes the genuine channel-mapping component its FIXME asks for;
  `SStdMixer`'s width comes from the project and is persisted.

**AC B4.1** Master page channel *k* equals the sum of the tracks' channel *k*,
for all k, at several positions.
**AC B4.2** Runtime width change 2→8→2 renders correctly and does not assert,
crash or leak; `repeat_test.sh` N=50 across workers {1,4,8,16}.
**AC B4.3** `plugins_test`, `plugins_scan_test` and every `plugin_*` qxa case
green; the `twtestclap` skew fixture proves cross-channel flow through the new
wide insert.
**AC B4.4** A plugin sees all channels in one `process()` call (report-block-size
and skew fixtures), and the mismatch table still holds at widths 1, 2 and 6.
**AC B4.5** Mixed-width safety (§4.5): a stale page of the wrong width is a miss,
proven by a test that forces one — including on the RT path and in `twLevelProbe`.
**AC B4.6** The **legacy pull path** is exercised wide at least once
(`SMARAGD_REVAL_WORKERS=0`), since `assert-meter` drives it and no AC elsewhere
covers it.
**AC B4.7** A plain stereo clip yields two distinct channels **at the track's
root component** — the assertion B3 could not make, because the track path was
still width 1 there.
**AC B4.8** Width-1 projects byte-exact throughout — **with one licensed
exception, `mc_golden_mono.wav`, re-frozen once here.**

*This AC and B4's third bullet cannot both hold for a mono project containing a
channel-mismatched plugin, and B4 stopped and said so rather than choosing.
Licensed, with the explanation §5's standing gate requires:*

*A `channels='1'` project running a 2-in/2-out plugin now takes **MonoFold** —
proposal 08's settled channel-mismatch table, re-derived from page width exactly
as B4 requires. The delta is **exactly 2/3 on every sample inside the
`twtestclap` window and byte-identical everywhere else**:
`0.5·(1.5gx + 0.5gx) = 1.0gx` against `1.5gx`. **The old bytes were never a mono
project's output.** Until B4 the project's width reached no track at all — M1's
defining constraint was that it must not — so every track ran two buses and the
master discarded one; `channels='1'` was an inert attribute, not a signal path.
The golden froze that state. `mc_golden_stereo` staying byte-identical while only
the mono golden moved is the independent signal that what changed is the mismatch
path and not the width plumbing.*

*Note this is a **user-visible behaviour change**, and a corrective one: a mono
project with a stereo plugin used to be rendered as if it were stereo.*

*And the re-freeze turned up the same finding from the other side: 970 of the
57 600 samples deviate from exactly 2/3 because **the old render was clipped at
full scale** — `1.5·g·x` overflows 16-bit where MonoFold's `1.0·g·x` does not.
The pre-B4 mono golden was not merely a different mapping; it was a
**saturating** one. Confinement was checked rather than assumed: 114 222 bytes
differ, first at 643 244 and last at 758 443 — the documented plugin window to
the byte at both ends — with the header and the plain / stretched / pitched /
container-asset / nested-lane windows byte-identical, and 56 630 of 57 600
samples within 0.333 LSB of `old·2/3`.*

### B5 — The sink goes wide ✅ **EXECUTED 2026-08-16** *(first audible multichannel)*

> `AudioFrame` is **deleted** — `float channels[2]` in `tw/core` was the hard
> stereo cap every sink and the engine saw; `AudioSink` is a block interface now
> (`writeFrames(interleaved, nFrames, channels)`), so width travels with the
> call. `pullBlock` takes N planar buffers with the §4.4 clamp per channel, one
> resampler per channel; `RenderSession` interleaves from one wide root page.
>
> **The device rule (requester decision):** `L = ch0; R = (width >= 2) ? ch1 :
> ch0` — mono-to-stereo at or below two channels, **first two only** above it,
> the rest computed and dropped at the device. It is the **device path only**;
> render and monitor share no code, so a 6-channel project renders six channels
> *and* monitors in stereo, and neither can move the other. Logged once per
> width, not per callback; asserted at width 6 → a 6-channel *device* too, which
> is the assertion that stops a later refactor fanning channel 2 into output 2.

`AudioEngine::pullBlock` takes N buffers; per-channel resamplers; `AudioFrame`'s
2-cap and `FileSink`'s frame-at-a-time write retired; `RenderSession` interleaves
from one wide root page; `RenderParams.channels`; writers fed the real count;
`twSpeaker` interleaves N instead of `(c % 2 == 0) ? sL : sR`.

**AC B5.1** `plugin_stereo_chain.qxa` tightened to the numbers **the case itself
already documents for this day**: channel 1 → `[0.030, 0.037]`, channel 0
unchanged.
**AC B5.2** A **plain** stereo clip rendered to WAV has genuinely different
channels (`assert-channels-differ`). This is why B3 precedes B5: without the clip
path wide, the only cross-channel content that exists is a plugin's, and this AC
would be asserting the plugin fixture rather than the signal flow. Capture-backed
clips are explicitly excluded here and covered by B7.
**AC B5.3** A 6-channel project renders a 6-channel file with per-channel
energies in band.
**AC B5.4** `dump-playback-capture` shows the same asymmetry as the offline
render at the same positions.
**AC B5.5** ~~Mono byte-exactness holds. The `channels=2` golden may change once
here.~~ **Both goldens change here, and both are licensed** — corrected before B5
started, because as written this AC contradicted **AC B5.3**: if a 6-channel
project renders a 6-channel file then `RenderParams.channels` comes from the
project, and a `channels='1'` project therefore renders a **one-channel** file.
`mc_golden_mono.wav` is a *two*-channel file today (the sink hardcodes 2 and
duplicates), so its size roughly halves and byte-exactness is arithmetically
impossible. Do not preserve it by keeping a mono project's file stereo — that
would be the sink still lying, which is the whole thing this milestone removes.

Expected, and each to be **verified rather than assumed**, in B4's manner:

- **`mc_golden_mono.wav`: a SHAPE change.** 2 channels → 1. Assert the new file's
  channel count and that its samples equal the old file's channel 0 — if the
  surviving channel is not exactly what channel 0 was, something else moved.
- **`mc_golden_stereo.wav`: a CONTENT change.** Channel 0 byte-identical to the
  old file's channel 0; channel 1 becomes real bus-1 audio instead of a duplicate.
  Report where it first differs and confirm channel 0 did not.
- Determinism first: render twice into separate output dirs and confirm the two
  agree **before** freezing either, as B1a and B4 both did.

*(Note the mono golden is being re-frozen a second time — B4 re-froze it for the
MonoFold correction. Two licensed re-freezes of one file in two milestones is
exactly why each needs its reason recorded beside the case, not only in a commit
message.)*

### B7 — Container/asset clips and the preview capture keep their channels

*(Rescoped by B3, which found that sample-backed stretch and pitch never touch
`buildCapture_` at all — see §2 item 2. What is left here is genuinely
capture-backed content plus the preview path.)*

`SCut::buildCapture_` captures N channels; the container/asset render path
renders per channel.

**AC B7.1** A stereo file stretched 1.25× still has distinct channels at the sink
(RMS discriminator).
**AC B7.2** Same for a pitched clip and a container/asset clip.
**AC B7.3** `grain_*`/`warp_*`/`exact_*` green; mono byte-exact.

### B8 — Metering, preview, UI stop lying

N-lane metering (`twLevelSample`/`twScanSpan`/`SLevelMeter` are scalar **by
type** — a widget and probe change, not config), fitted into the track head's
density rules (the 120 px column has ~13 px of slack: a second lane is a layout
decision, not a squeeze); render-dialog channel control; preview per-channel or
an explicitly documented fold — either way bump `twAspect::PreviewPeaksVersion`,
because the sidecar key asserts `qi.channels = 1` today and every existing
sidecar would otherwise mis-hit.

**AC B8.1** `metering_test` extended; per-lane ballistics stay frame-rate
independent.
**AC B8.2** `meter_levels` asserts a wide project's lanes read *differently*, with
PNG grabs.
**AC B8.3** Old sidecars miss rather than mis-read after the version bump.
**AC B8.4** Track head at 150/100/60/40 px and both column widths renders without
clipping — grabs attached to the PR.

### B9 — Contracts, cleanup, and the report

Amend `docs/contracts/FREEZE_PROTOCOL.md` §"Page geometry" (it currently says
65536 **mono** frames, normatively) and the affected `CONTRACT.md` files; delete
or wire the dead scaffolding (`twFormatCaps::channelCounts`, `twSpeaker`'s unread
input plugs, `twConvertFrames`' planar refusal if a wire now needs it); publish
the 8-channel measurements (node count, pages in flight, memory, freeze
wall-clock, invalidation cost) that v1 would have gathered in its M6.

**AC B9.1** No contract in the tree still asserts a mono page.
**AC B9.2** Measurements published in this proposal; node count at 8 channels is
within noise of the width-1 count (B's central claim, now evidenced or refuted).
**AC B9.3** Full suite green; `repeat_test.sh` on the DSP-sensitive set.

---

## 6. Execution

- **Workspace:** worktree `.claude/worktrees/multichannel`, branch
  `feat/multichannel`. One PR per milestone; never a direct push to `main`.
- **Agents:** one Opus agent per milestone, given this proposal, its milestone's
  ACs as the definition of done, and the standing gate list. **An agent that
  cannot make an AC true stops and reports — it does not weaken the AC.** v2's
  review found exactly the failure mode this guards against: a milestone whose
  ACs could not all be true at once, which an agent would have "resolved" by
  weakening whichever was closer.
- Order: **M0, M1** (independent of each other and of B1) → **B1 → B2 → B3 → B4
  → B5 → B7 → B8 → B9**, strictly serial from B1 on. Each phase's safety comes
  from the previous one's gate. (v2's B6 is folded into B4; the numbering is kept
  so the review's findings stay traceable.)
- **User-visible state while half-built:** from M1 the project carries a channel
  count that does nothing audible until B5. The render dialog and options UI
  **must not expose it before B5** — a control that silently does nothing is
  worse than no control. B8 exposes it.
- **The suite is the only safety net** — there is no CI.

## 7. Known traps, collected

1. `assert-audio-*` ignores `channel=` when `frameCount="-1"` (M0) — until then no
   channel assertion in this repo means what it appears to mean.
2. `SObject::recordingChannels_` is live in the UI and plumbed to
   `RecordingParams`, but **never serialized** — a per-track channel selection
   dies on save. `RecordingParams.channels` is hard-coded 2 in `SMainWindow`.
3. `SObject::pan_` is serialized into every `.qxp` with zero consumers. Removing
   it breaks `assert-project-matches` goldens; giving it meaning is proposal 36.
   Do neither by accident.
4. `twWavInput::getNOutputs()` returns a hardcoded **4** while only latch 0 is
   created (B3).
5. `twFormatCaps::channelCounts` is declared and never populated; the negotiator
   has zero channel logic. Wire it or delete it (B9) — leaving it is how the next
   reader concludes width is negotiated when it is not.
6. `twConvertFrames` refuses planar layouts. If a planar buffer ever crosses a
   wire, this is the boundary that fails first.
7. The `updateClip` duplicate-key class of bug is documented as *"a trap primed
   for the stereo-output work"*. Expect it live in B4; the fix pattern is
   "invalidate over the union of matching keys".
8. `getNOutputs()` means three different things in three classes today. B2
   introduces `getOutputChannels()` precisely so it can keep meaning ports —
   resist every temptation to merge them.
9. **`Q_ASSERT_X` is compiled OUT of the build everyone runs** (found by M1).
   `smaragd/CMakeLists.txt:21` strips `-DNDEBUG` from RelWithDebInfo to keep the
   engine's own asserts, but Qt still defines `QT_NO_DEBUG`, so every `Q_ASSERT*`
   vanishes. `STrack::setNBusses`'s shrink therefore does **not** "assert the app
   dead" — it returns *silently*, leaving stale wiring and saying nothing, which
   is the worse failure and is why the `nBusses` drift stayed invisible (the
   resulting count was right by accident). **B4 must not rely on a `Q_ASSERT` to
   catch a width mistake**, and §4.3's "the base implementation asserts" must be a
   real runtime check plus `TW_LOG`, not `Q_ASSERT_X`.
10. **`load-project` deserializes INTO the current project instead of replacing
    it** (`sloadprojectaction.cpp`; found by M1). The GUI's File→Open builds a
    fresh `SProject` and swaps; the action does not. So a `.qxa`
    save→load→save accumulates the previous arrangement plus zero-ref orphan
    mixers and plugin chains (2157 → 4186 bytes in M1's case). This contradicts
    `main/objects/track/CONTRACT.md` inv. 7's claim that holding a chain
    reference "is what lets a save/load/save comparison be byte-equivalent".
    Pre-existing and unrelated to channels — but **B1's golden corpus must not be
    built on a save→load→save round trip** until it is fixed.
11. **`tests/repeat_test.sh` must be run from `tests/cases/`**, despite its header
    claiming it works from any directory (found by M1). Elsewhere, relative
    `load-project`/`save-project`/`assert-file-contains` paths resolve against the
    CWD and a perfectly good case reports 0/10. Do not read that as a flake.
12. **`releaseOldPages` has no caller anywhere** (found by B1a). Its retention bug
    was therefore latent, and the fix provably changes no runtime number — but the
    reason it was latent is the finding: **`outputPages_` is pruned only by
    invalidation and teardown, so it grows unbounded across a session.** Wiring it
    up (or retiring it) is B9 work, and it gets *worse* with width — 2 MiB per
    retained page at width 8. If memory bites earlier than B9, this is the cause
    to look at first.
13. **`SProject::getDurationSeconds()` is a hard-coded `return 60.0;`** with a
    "TODO: calculate from arrangement" (found by B1a). **Every render in the
    suite is 60 s regardless of content** — the 4 s corpus rendered 11.5 MB, 93%
    of it silence. B1a added an optional `durationSec=` to `render` (default -1 =
    today's behaviour, every existing case untouched) rather than change the
    length of every render in the suite. Any timing or memory measurement taken
    from a render must account for this, including B9.2's.
14. **The two goldens are byte-identical to each other today** (found by B1a),
    because `RenderSession` hard-codes `config.channels = 2` and duplicates one
    mono bus, so a width-1 and a width-2 project produce the same file. That is
    correct *now* — and it is a second gate that is **supposed to break at B5**,
    alongside `channel_assert_dupmono.qxa`. When they diverge, the sink is real.
    **⚠ SPENT AT B4 — DO NOT USE THIS INFERENCE.** The goldens diverged at B4
    (114 222 bytes, all inside the `twtestclap` window) **while the sink was
    still mono**, because channel 0 *itself* now differs between a mono and a
    stereo project: the mismatch table folds a 2-in/2-out plugin on a width-1
    project and does not on a width-2 one. Divergence therefore no longer implies
    anything about the sink. **B5 needs different evidence**;
    `channel_assert_dupmono.qxa`'s `expectReject` is untouched and still serves.
    *(The general lesson is worth more than the instance: a gate whose meaning is
    "these two things are equal for a reason" quietly expires when the reason
    changes, and it expires silently — the gate still passes, or still fails, for
    a different reason than the one written down.)*
15. **A missing `qoffscreen.dll` costs 600 s, not a fast failure** (found by B1b).
    `preview_container_test` and `project_channels_test` both set
    `QT_QPA_PLATFORM=offscreen` and both construct a `QApplication`;
    `windeployqt` deploys only `qwindows.dll`, so Qt blocks in an invisible
    platform-plugin dialog — **0.03 s of CPU over a 300 s timeout, each**. Copy it
    from the Qt prefix into `smaragd/build/bin/platforms/`, and **re-copy after
    every `./rebuild.sh`**, which wipes `build/bin`. A green suite reported by an
    agent that had the plugin is not evidence for one that does not.
16. **`twlog_test` asserts a wall-clock bound and therefore measures the machine**
    (found by B1b): 137-191 µs idle, **4 000-51 000 µs at 100% CPU, failing 6 runs
    in 10**, same binary. It is the one wall-clock assertion in the suite. Never
    diagnose it as a code regression without checking the load first, and note
    that it is fundamentally hostile to a parallel test run.
17. **`warp_anchors_roundtrip` has a pre-existing teardown segfault**, ~1 in 30 at
    `SMARAGD_REVAL_WORKERS=1`, load-sensitive (found by B1b, bisected below B1a's
    work). Exit 139 inside `~SProject`, *after* all audio work: the case leaves
    **two objects that outlived the refcount cascade with dangling `SLink`s** on
    every run, pass or fail, and occasionally the teardown order kills it. That is
    the known `~SLink`-must-`setParent(nullptr)`-first UAF class. A *second,
    distinct* mode appears at workers=8: `SRenderAction: render timeout after
    30000 ms` at 96% of the render — a wall-clock budget blown on a loaded box,
    aggravated by trap 13. The exit code separates them; do not conflate them.
18. **`renderFrames()` and `calcOutputTo()` are mutually recursive in the base
    class** (found by B2, by mutation: stack exhaustion, `0xC00000FD`). Base
    `renderFrames` calls `calcOutputTo`; base `calcOutputTo` calls `renderFrames`.
    A component overriding **neither** recurses until the stack ends. Pre-existing
    and harmless today because everything overrides one — but **B3 and B4 write
    wide components**, and a wide-only component handed a mono scratch page walks
    straight into it. **A wide component must keep a narrow `renderFrames()`
    degradation.**
19. **Three components override `freezePage` and allocate their OWN pages**,
    bypassing the width wiring entirely: `twTrackMix`, `twPluginChain`,
    `twPluginInsert` (found by B2; all three sites marked in `415efc4`). Because
    the render fork is on the *page's* width, a component that declares 4 but
    hands itself a width-1 page renders channel 0 and publishes it **with no
    refusal**. §4.5 catches it downstream as a miss — one log line and silence,
    not garbage — so it is findable, but it is B4's job to actually fix the
    allocation. Expect it; do not spend an afternoon on it.
    **Fourth site, found by B4:** the *preview* branch of
    `twPluginInsert::freezePage` allocates its own page too, and must copy
    **every** channel with the clamp — copying only channel 0 leaves a wide
    preview page carrying garbage past it.
20. **`idx_t` is `short int` in this tree.** Relevant whenever a channel count is
    cast or compared.
21. **A latent buffer overrun in the legacy pull, found by B3 and NOT fixed.**
    `twComponent::calcOutputTo(sample_t*, length, idx)` calls
    `resizeMonoScratch(length)` and then wraps the page with
    `IOVector::CreateForPageOutput`, which **hard-codes `FRAME_CAPACITY`** as the
    length. For any `length < FRAME_CAPACITY` the component is asked for a whole
    page, its cursor advances a whole page, and the `memcpy` back **reads past
    the scratch buffer**. Latent only because the freeze path calls it with a
    full page every time; B3 hit it by asking for 4096 frames and re-aimed its
    test at a full page rather than pin a bug it was not chartered to fix. Same
    class as trap 18, and it will bite whoever widens the legacy pull.
22. **`test_sawtooth.wav` is a TWO-CHANNEL file whose channels are byte-identical**
    (found by B3), and 80 of the 89 pre-existing cases use it. So from B3 onward
    **every clip in the suite freezes a width-2 page** — a far broader exercise of
    the wide path than any new case could buy, and the reason the byte gate stays
    green through it. It is also why a memory or page-count number moves at B3
    without anything having gone wrong: the corpus went 12.25 → 14.0 MiB, exactly
    7 of 49 resident pages now being 2 channels wide.
    **Consequence found at B5:** this is *also* why `channel_assert_dupmono`
    could never be the B5 signal it was designed as. Its render's channels are
    equal **after** the sink went wide, for the same reason they are equal in the
    source. A gate whose fixture cannot distinguish the two states is not a gate
    for that distinction — it is now a *pair* (sawtooth equal / `test_stereo`
    different, one project, one render), and only the real thing passes both.
23. **`twPluginInsert::calcOutputTo` can feed a plugin only channel 0** (found by
    B4). A plug pull is mono by construction and an insert now has one plug, so
    the streaming-pull path cannot present a wide input. **Nothing in the app
    reaches it** — which is the difference between debt and a bug — and it is
    recorded in `plugins/CONTRACT.md` known debt.
24. **Plugin scanning is CROSS-WORKTREE state, and a rescan can hang the whole
    suite** (found by B5). `kScannerVersion` lives in the source while
    `plugincache.json` lives once per USER, so a worktree carrying a bumped
    version invalidates the cache for every other worktree — which then re-probes
    every installed plugin on every start. On a machine with real plugins that is
    slow enough to reach static destruction with the scan thread alive, and
    `~twPluginRegistry` → `waitForScan()` → `QThread::wait()` then blocks
    forever: every `--test-case` run passes and **hangs at exit**, dying on
    CTest's 600 s timeout. Reproduced with none of B5's changes. Same shape as
    trap 15: an environmental fact that costs whole gate runs and reads as a
    suite problem. Workaround: point `[plugins] searchPaths` at
    `smaragd/build/bin` (where the fixtures are). Real fix: unify the version
    across worktrees, or land the teardown fix — an unmerged
    `fix/plugin-scan-teardown-hang` branch already exists.

## 8. Non-goals (named, so they are not assumed)

- **Panning.** No pan law, no panner stage; `SObject::pan_` stays dead until
  proposal 36. A stereo project with mono sources is dual-mono — correct, and not
  yet musical. This is the single most likely "but it doesn't *do* anything yet"
  reaction, and it is by design.
- **A routing matrix** for plugins matching no width (they stay bypassed, per 08).
- **Surround semantics** — channel roles (L/R/C/LFE/Ls/Rs), fold-down presets,
  ambisonics. This delivers N channels, not a 5.1 *format*.
- **Multichannel MIDI/instruments**, per-clip channel routing, and interleaved
  page layouts (`twLayout::Planar` is the engine's internal shape by §4.1).
