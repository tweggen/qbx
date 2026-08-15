# Proposal 35 — Configurable multichannel signal flow (mono / stereo / 4 / 6 / 8)

> **Status: DRAFT (2026-08-15).** No code yet. This proposal answers a gap that
> `plan/STATE.md:7741` recorded and left unowned: *"The multi-channel sink above
> is nobody's milestone yet and **should become one**."* It is now one.
>
> Prerequisite reading: `plan/proposed/04_WIRE_FORMAT_AND_SAMPLE_RATE.md`
> (`twFormat` already carries `channels`/`layout`),
> `plan/proposed/08_PLUGIN_HOSTING.md` §"Design premise 2" and §"Settled
> decisions 5, 6" (the parallel-mono-wires decision and the channel-mismatch
> policy), `docs/contracts/FREEZE_PROTOCOL.md` §"Page geometry",
> `plan/proposed/19_ASYNC_FREEZE_MODEL.md` ("Phase 2 REVISED"),
> `smaragd/tw303a/plugins/CONTRACT.md:364-372`,
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

Notably `plan/proposed/29_FOLLOWUPS.md` — the "what's next" routing document —
does not mention channel width at all. So: the ground is prepared, the gap is
recorded everywhere it bites, and nobody owns it.

---

## 2. What is actually true today (and it is not "the engine is mono")

The received summary is *"the engine is mono"*. That is wrong in a way that
changes the plan, so state it precisely:

**Already N-channel, working, tested:**

- **Every source.** `twSampleSource` decodes to planar Float32
  (`channel c, frame f at data_[c*nFrames_ + f]`), and so do `twResampledSource`,
  `twGrainSource` and `twPagedVocoder` (whose `Config.channels` is exercised at 2
  in its own tests).
- **The reader already treats `idx` as a channel.**
  `twSampleReader::getNOutputs()` returns `src_.channels()` and builds one latch
  per channel; `calcOutputTo(dest, idx)` calls `src_.read(pos_, …, idx)`. This is
  the one place in the engine where a port index *is* a channel index.
- **A track really is two buses.** `STrack::setNBusses(2)` in the ctor builds two
  `twTrackMix` + two `twPluginChain` instances joined by a 2-plug `twRewire`;
  gain fans out over both; the plugin taps render both.
- **The file writers, the WASAPI backend, the analysis layer and
  `dump-playback-capture` are all genuinely N-channel** and merely pinned to 2.

**So on a 2-bus track a stereo WAV already produces two genuinely different
buses.** The audio exists. It is destroyed downstream, at three chokepoints:

1. **The master**, `SStdMixer::setNBusses(1)` (ctor, hard-coded, never
   serialized): the summing loop runs `bus < 1`, so every track's bus 1 is
   built, filtered, plugin-processed — and dropped. `getRootComponent()` still
   carries `// FIXME: Generate a channel reassignment.`
2. **The sink**, twice: `RenderSession` (`config.channels = 2;` +
   `bufR[i] = sample;  // Duplicate to stereo (temporary; proper multi-channel
   TBD)`) and `AudioEngine::pullBlock(outL, outR, n)` (`// Duplicate mono frozen
   output to stereo`), sitting on `AudioFrame::MAX_CHANNELS = 2`. `twSpeaker`
   then fans out `(c % 2 == 0) ? sL : sR`.
3. **The clip capture**, `SCut::buildCapture_`: reads channel `0` and builds a
   `twCapturingSource(…, 1, …)`. **Any stretched, pitched or container-backed
   clip becomes mono**, whatever the file was. This one is not at the sink at
   all, and it is the least-known of the three.

Plus one structural blocker: **channel width cannot change at runtime.**
`STrack::setNBusses` grows only — shrink is `Q_ASSERT_X(false, …, "bus count
shrink not supported")` — and the loader's default (`nBusses="1"`) contradicts
the ctor's 2.

---

## 3. The architectural question this forces

Proposal 08 settled it once: **N channels = N parallel mono wires = N parallel
component instances**, because the frozen page is mono by contract. That is not a
style preference — it is forced by the page model, and every layer is built on
it:

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

There is **no bus dimension anywhere in the freeze, cache, demand or invalidation
machinery.** Nothing needs one, *as long as* N channels are N components.

The cost of that decision is already visible in the tree, and it is worth being
blunt about it before extending it to 8 channels:

- **`twPluginSlotProcessor` exists only to work around it.** Its header:
  *"It is deliberately NOT a twComponent… so N parallel mono wires are N parallel
  component instances"*, and `twplugininsert.h`: *"a component that wrote
  interleaved stereo into one page produced garbage the engine then read as
  mono."* Every channel-coherent stage needs this out-of-band processor + per-bus
  tap + private cache pattern.
- **Per-bus instantiation has already produced bugs**: the `setNBusses` grow
  crash and shrink assert; the pre-M3 `twPluginChain` loop that fed a 2-in plugin
  "bus audio on input 0 and SILENCE on input 1"; the nested-lane duplicate-key
  trap explicitly logged as *"primed for the stereo-output work"*.
- **It multiplies the DAG.** 8 channels = 8× nodes, 8× dependency counting, 8×
  epoch bookkeeping, 8× invalidation walks, 8× `twTrackMix`/`twPluginChain`
  instances per track.

### Option A — extend the current model (parallel wires all the way out)

Widen `SStdMixer`, the sink, the capture and the UI; keep one mono page per
component and one component per channel.

- Page contract untouched → the render byte-`cmp` gate is safe by construction
  for mono, and stays meaningful throughout.
- Every milestone is independently shippable and gateable.
- The graph half is *already proven at N=2*; this is finishing it, not inventing.
- **But** every future channel-coherent stage (panner, stereo width, M/S,
  correlation metering, linked dynamics) needs the processor/tap workaround, and
  the DAG grows linearly in channels.

### Option B — make the page multichannel (planar, page carries `channels`)

One component = one page holding N planar channels; components declare width.

- The scheduler key `(component, position)` stays *exactly as it is* — no node
  explosion, no bus dimension to add.
- The planar source side flows straight through; `twSampleReader`'s per-channel
  latches and the processor/tap split both become unnecessary.
- Channel-coherent DSP becomes ordinary, not a special case.
- **But** it changes the single most load-bearing contract in the engine
  (`FREEZE_PROTOCOL` page geometry) and touches every `freezePage` override, the
  page pool, the latch seam, capture, sidecar keys, metering and preview *at
  once*, with byte-exactness verifiable only afterwards. It is a stop-the-world
  change to the layer that caches, invalidates, stales and meters everything.

### Option C — keep mono pages, promote the tap pattern to a facility

Extract `twPluginSlotProcessor`/`twPluginInsert` into a reusable
`twWideProcessor` + `twWideTap` pair that any channel-coherent stage can use.
Not an alternative to A — a component of it.

### Recommendation

**A + C now; B behind an explicit decision gate (M6).**

The reasoning is not "A is nicer". It is that A reaches the user-visible goal —
configurable mono/stereo/4/6/8 with real audio on every channel — **without
touching the contract that every correctness gate in this repo is anchored to**,
and the graph half of A is already working at N=2. B is the better long-run
model, and this proposal says so; but it should be entered with measurements in
hand (M6), not speculatively, and as its own proposal. Adopting B *first* would
mean rewriting the page model before a single channel of audio has ever reached a
file — i.e. taking the largest risk before validating any of the value.

**If the requester prefers B up front, M0–M2 are unchanged and still required;
M3–M5 would be re-planned.** That is the fork, and it is recorded here so the
choice is deliberate.

---

## 4. Goals / non-goals

**Goals**

- A project-level channel count: 1, 2, 4, 6, 8. Persisted, changeable, defaulting
  to 2 for legacy projects (which is what they sound like today).
- Every track carries that many buses; sources with N channels reach the master,
  the file and the device with their channels intact.
- Stretched / pitched / container-backed clips keep their channels.
- Renders and playback produce genuinely N-channel output, gated by assertions
  that can tell the channels apart.
- Metering, preview and the render dialog stop lying about width.

**Non-goals (named, so they are not assumed)**

- **Panning.** `SObject::pan_` is serialized into every `.qxp` and has *zero*
  consumers — no action, no UI, no DSP. A pan law + panner stage is a separate
  proposal (36). Without it, a stereo project with mono sources is dual-mono:
  correct, and not yet musical.
- **A routing matrix** for plugins whose I/O matches no bus count (proposal 08
  left these bypassed; they stay bypassed).
- **Surround semantics** — channel *roles* (L/R/C/LFE/Ls/Rs), fold-down presets,
  ambisonics. This proposal delivers N channels, not a 5.1 *format*.
- **Multichannel MIDI/instruments**, and per-clip channel routing.
- **Changing `twOutputPage`** (that is M6's question, not this proposal's work).

---

## 5. Milestones and acceptance criteria

Every milestone: `./build.sh` + `python tools/check_layering.py` +
`python tools/check_logging.py` + full `ctest` green, and **a mono (channels=1)
and a stereo-today (channels=2) project must render byte-identically to their
pre-milestone goldens** (`cmp` on 16-bit PCM WAVs) unless the milestone's AC
explicitly says otherwise. That standing gate is written once here and assumed
below.

### M0 — Make the gates able to see channels (no behaviour change)

The suite currently *cannot* detect the feature landing or regressing.

1. Fix `assert-audio-energy` / `assert-audio-peak`: `channel=` is **silently
   ignored when `frameCount="-1"`** (both take the whole-file path, which
   hard-codes `channelIndex = -1`, the all-channel mean). Today this is invisible
   because the channels are equal; it would start silently mis-passing the day
   the sink goes wide.
2. Add a discriminator verb — `assert-channels-differ filename= channelA=
   channelB= minRmsDelta=` (or equivalent) — so "the channels are genuinely
   different audio" is assertable rather than inferred.
3. Commit an asymmetric multichannel fixture (a 4-channel WAV whose channels
   carry different, known RMS).

**AC 0.1** The fixed verbs FAIL on the asymmetric fixture with a wrong `channel=`
and PASS with the right one, with `frameCount` both given and omitted.
**AC 0.2** `assert-channels-differ` fails on a duplicated-mono render and passes
on the fixture.
**AC 0.3** Every existing `.qxa` case still passes unchanged (all current
`channel=` users also pass `frameCount=`, so the fix is behaviour-preserving for
them — verify, do not assume).

### M1 — Channel count becomes project data (still inaudible)

- `SProject::channels()` + `<SProject … channels='N'>`, read with the
  `sampleRate` **warn-and-default** idiom (missing ⇒ 2 + a warning), propagated
  to `tw303aEnvironment` the way `setSRate` is.
- `set-project-channels` action; `docs/ACTIONS.md` updated (it is hand-maintained
  and must stay truthful).
- `STrack::setNBusses` **shrink** implemented (no stale wiring, no
  use-after-free), and the loader/ctor drift fixed (`nBusses="1"` default vs 2).

**AC 1.1** Save→load→save is byte-equivalent (`persistence/CONTRACT.md` inv. 4;
`serialization_roundtrip_test` green).
**AC 1.2** A legacy `.qxp` with no `channels=` loads as 2 and warns once.
**AC 1.3** A project saved at `channels='6'` reloads with 6 buses on every track,
including nested ones.
**AC 1.4** 8 → 2 shrink, then a render, does not assert, crash, or leak; run
under `repeat_test.sh` N=50 across `SMARAGD_REVAL_WORKERS` {1,4,8,16}.

### M2 — The master carries N buses (sink still mono)

- `SStdMixer` bus count from the project (persisted, not a ctor constant); the
  summing loop covers every bus; the `getRootComponent()` FIXME resolved;
  `SApplication::rewireSpeaker`'s `if(root->getNOutputs() > 1)` branch becomes
  live rather than dead.

**AC 2.1** An engine-level test asserts master bus *k*'s page equals the sum of
the tracks' bus *k* pages, for k in [0,N), at several positions.
**AC 2.2** `plugins_test`'s "the two buses are genuinely different audio" still
green; `plugin_stereo_chain.qxa` still passes with its current (wide) bands.
**AC 2.3** Standing byte-exactness gate holds for channels=1 and channels=2.

### M3 — The sink goes wide (the payoff)

- `AudioEngine::pullBlock` takes N buffers (or an `IOVector` of them); per-channel
  resamplers; `AudioFrame`'s 2-cap replaced or retired along with `FileSink`'s
  frame-at-a-time write.
- `RenderSession` pulls one root page per channel and interleaves; `RenderParams`
  gains `channels`; the writers get the real count.
- `twSpeaker` interleaves N properly instead of `(c % 2 == 0) ? sL : sR`, and its
  vestigial input plugs are resolved.

**AC 3.1** `plugin_stereo_chain.qxa` tightened to the numbers **the case itself
already documents** for this day: channel 1 → `[0.030, 0.037]`, channel 0
unchanged. (The case says so in a comment; make it true.)
**AC 3.2** A stereo file rendered to WAV has genuinely different channels
(`assert-channels-differ`), via a cross-channel fixture — never `L != R` on a
path that could still be duplicating.
**AC 3.3** A 6-channel project renders a 6-channel file whose per-channel
energies match expectation within band.
**AC 3.4** `dump-playback-capture` shows the same asymmetry as the offline render
at the same positions (playback and render agree).
**AC 3.5** Standing byte-exactness gate holds for channels=1; for channels=2 the
golden is **allowed to change once**, and the change must be explained (bus 1 is
new audio, not a regression) and re-frozen.

### M4 — Clips keep their channels

- `SCut::buildCapture_` captures N channels (`twCapturingSource` with N), the
  container/asset render path renders per channel, and `channels` is threaded
  into the grain/vocoder configs (both are already channel-aware).

**AC 4.1** A stereo file stretched 1.25× still has distinct channels at the sink
(RMS discriminator, not `L != R`).
**AC 4.2** The same for a pitched clip and for a container/asset clip.
**AC 4.3** All `grain_*`, `warp_*`, `exact_*` cases green; mono byte-exactness
holds; sidecar/capture cache keys that encode channel count are bumped so no old
entry can be a wrong-shape hit.

### M5 — Metering, preview, UI stop lying

- N-lane metering (`twLevelSample`/`twScanSpan`/`SLevelMeter` are scalar **by
  type** — this is a widget + probe change, not a config change), fitted into the
  track head's density rules (the 120 px column has ~13 px of slack: a second
  lane needs a real layout decision, not a squeeze).
- Render dialog channel control (needs `RenderParams.channels` from M3).
- Preview: per-channel or an explicitly documented fold — either way bump
  `twAspect::PreviewPeaksVersion`, because the sidecar key currently asserts
  `qi.channels = 1` and every existing sidecar would otherwise mis-hit.

**AC 5.1** `metering_test` extended: per-lane ballistics remain frame-rate
independent.
**AC 5.2** `meter_levels` asserts that a wide project's lanes read *differently*,
plus PNG grabs of the multi-lane meter.
**AC 5.3** Old sidecars are ignored rather than mis-read after the version bump
(assert a miss, not a wrong hit).
**AC 5.4** Track head at every density (150/100/60/40 px) and both column widths
renders without clipping — grabs attached to the PR.

### M6 — Decision gate on the page model (spike, nothing ships)

Measure at 8 channels: scheduler node count, pages in flight, memory, freeze
wall-clock, invalidation cost. Prototype `twOutputPage.channels` far enough to
size the change.

**AC 6.1** A written recommendation with numbers appended to this proposal.
**AC 6.2** No merge to `main` from this milestone; if B is recommended it becomes
proposal 37 with its own milestones.

---

## 6. Execution

- **Workspace:** worktree `.claude/worktrees/multichannel`, branch
  `feat/multichannel`, already created and building green. One PR per milestone
  off that branch (or a stacked branch per milestone), never a direct push to
  `main`.
- **Agents:** one Opus agent per milestone, each given (a) this proposal, (b) the
  milestone's ACs as its definition of done, (c) the standing gate list from §5.
  An agent that cannot make an AC true stops and reports — it does not weaken the
  AC. Milestones are ordered; M0 and M1 may run concurrently, everything after
  M2 is serial.
- **The suite is the only safety net** (there is no CI). Every milestone re-runs
  `cmake` configure (the qxa glob is `CONFIGURE_DEPENDS`) and reconciles
  registered vs run vs skipped case counts.
- **A case that fails once and passes on re-run is not a pass** — pin with
  `repeat_test.sh`, swept over `SMARAGD_REVAL_WORKERS` {1,4,8,16}, and report
  either way.

## 7. Known traps, collected

1. `assert-audio-*` ignores `channel=` when `frameCount="-1"` (M0 fixes it) —
   until then, no channel assertion in this repo means what it appears to mean.
2. `SObject::recordingChannels_` is live in the UI and plumbed to
   `RecordingParams`, but **never serialized** — a per-track channel selection
   silently dies on save. Adjacent, half-built, worth finishing in M1.
3. `RecordingParams.channels` is hard-coded 2 in `SMainWindow`.
4. `SObject::pan_` is dead weight in every `.qxp`. Removing it would break
   `assert-project-matches` goldens; giving it meaning is proposal 36. Do
   neither by accident.
5. `twWavInput::getNOutputs()` returns a hardcoded **4** while only latch 0 is
   created — an outright inconsistency that will bite the moment latches beyond 0
   are pulled.
6. `twFormatCaps::channelCounts` is declared and never populated; the negotiator
   has zero channel logic. Either wire it in M1 or delete it — leaving it is how
   the next reader concludes width is negotiated when it is not.
7. `twConvertFrames` refuses planar layouts (no wire uses them yet). If M4 moves
   planar data across a wire, this is the boundary that will fail first.
8. The `updateClip` / duplicate-key class of bug is documented as *"a trap primed
   for the stereo-output work"*. Expect it to go live in M2–M3; the fix pattern is
   "invalidate over the union of matching keys".
