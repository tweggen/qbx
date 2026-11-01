# Proposal 56 — NassauDigital: the sampler as a native instrument (track Q)

Status: proposed. Owner: unassigned. Ticket: QBX-240.
Touches: a new engine module `tw303a/sampler/`, a locator for the core
(`smaragd/cmake/NassauDigital.cmake`, no submodule), a new app slice `main/objects/sampler/`,
a new UI slice `main/samplerui/`, and small seams in `tw303a/plugins/`
(registry, slot processor), `main/objects/track/` (SPluginSlot),
`main/shell/` (run barrier, recorder), `main/pluginui/` (editor routing),
`tw303a/render/` (a memory writer), `tools/check_layering.py`, `ci/`,
`_env.sh` and `docs/ACTIONS.md`.

Prior art: proposal 37 (instruments, the event feed, the generator rows, the
run barrier, §5.4 multi-output), proposal 21 (the live lane), proposal 27 (the
vocoder and the onsets aspect), proposal 05/09 (live assets, arrangements),
proposal 53 (the record planner), proposal 08 (plugin hosting, the slot
processor, `setFactory`).

Depends on, outside this repository: **tweggen/nassau-digital**, whose
`docs/DESIGN.md` is the specification of the instrument and whose
`docs/GATES.md` is the schedule. Track Q of that schedule (Q0–Q8) is this
proposal's milestone list, one to one. Its `docs/REVIEW.md` is an independent
review whose file:line references into this tree date from 2026-10-26; every
one this proposal relies on was re-checked, and §10 lists the ones that turned
out wrong.

---

## Reading list, before any milestone

1. nassau-digital `docs/DESIGN.md`, all of it; §6.1, §6.2, §7, §9 above all.
   Where this proposal and DESIGN.md disagree about the INSTRUMENT, DESIGN.md
   wins. Where they disagree about QBX, this proposal is the one that read the
   code, and §10 says where.
2. nassau-digital `docs/GATES.md`, track Q, and the house rules R1–R11.
3. `smaragd/tw303a/plugins/CONTRACT.md` invariants 1, 15, 16 (the generator
   rows), 34, 38, 40, 42, 45, 46; `include/tw/plugins/twplugin.h`;
   `include/tw/plugins/twpluginslotproc.h`.
4. `docs/contracts/FREEZE_PROTOCOL.md`, "The run barrier".
5. `smaragd/main/objects/recplan/CONTRACT.md` (why `place-sampler-recording`
   is NOT a `PassVerbs` implementation, §4.10).
6. `smaragd/tw303a/playback/CONTRACT.md` invariants 11, 15, 16, 18 (the live
   lane and its rate refusal).
7. `smaragd/tw303a/sources/CONTRACT.md` (the vocoder's streaming mode,
   `viewAtRate`, the growing source) and `include/tw/sources/twpagedvocoder.h`.
8. `tools/check_layering.py`, `DEPS`, `APP_DEPS`, `APP_ENG`.

---

## 1. Problem

NassauDigital is the Nassau family's sampler: a dependency-free core (program
model, its XML format, the voice DSP; no Qt, no IPlug2, no SDK) in
tweggen/nassau-digital, whose **first front end is native in Smaragd**, as an
instrument of qbx's own `tw` format (DESIGN.md, "Where it lives",
**decided** 2026-10-26). Everything around a sampler already exists in qbx —
assets, recording, the stretch vocoder, undoable actions, `.qxa` scripts and
headless tests — and none of it can reach an instrument today:

* **The plugin ABI cannot carry a program or PCM.** `twPlugin` is
  prepare / process / reset / parameters / an opaque state blob
  (`smaragd/tw303a/plugins/include/tw/plugins/twplugin.h:44-69`), and it has
  no generic extension query; its only promise is that "format-specific
  behavior … lives behind capability-queried extension interfaces"
  (`twplugin.h:38-39`). Pushing samples through `loadState()` would put them
  into the project file: the slot keeps the blob verbatim
  (`smaragd/main/objects/track/include/app/objects/track/spluginslot.h:283`)
  and base64-writes it into the `.qxp` on every save
  (`smaragd/main/objects/track/src/spluginslot.cpp:385-389`).
* **A built-in instrument is minted by uid, by `tw_plugins` itself, with no
  handle to anything** (`smaragd/tw303a/plugins/src/twpluginregistry.cc:704-709`),
  and discovery is symbol-referenced by contract
  (`smaragd/tw303a/plugins/CONTRACT.md:62-65`). A sampler module that subclasses
  `twPlugin` must depend on `tw_plugins`, so `tw_plugins` cannot name it back.
* **Renders must stay byte-identical** while new audio for a layer arrives
  (a tempo change, an edited asset, finished onset analysis). Every instrument
  page is a pure function of position and feed
  (`docs/contracts/FREEZE_PROTOCOL.md:116-119`); a sample that changes under a
  render would make a render of two generations.
* **qbx has no sibling-repository dependency.** It has git submodules, but
  only third-party header SDKs (`.gitmodules`: `third_party/clap`,
  `third_party/vst3_pluginterfaces`). A Nassau core qbx compiles is new, for
  the build, for CI and for nassau-suite's release pinning; §4.1 takes it the
  way the plugin repos take nassau-plugin-sdk, by a locator, not a submodule.

This proposal designs the qbx side so that track Q can be built gate by gate,
each gate one qbx PR held to `./ci/gates.sh` as well as to its ACs.

---

## 2. What exists, re-verified on 2026-10-26

| Fact | Where |
|---|---|
| The built-in 303 is a `twPlugin` subclass linked into `tw_plugins`, mono out | `smaragd/tw303a/plugins/src/twnativeinstrument.cc:111-116` |
| Its state blob is parameters only, never the voice | `twnativeinstrument.cc:222-238` |
| The registry appends the built-ins and mints them by uid | `twpluginregistry.cc:292-332`, `:704-709` |
| Aux output buses are discovered, not routed | `twplugin.h:113-130`; `plugins/CONTRACT.md:511-517` (inv. 34) |
| The slot processor hands a generator **one** output bus, whatever the plugin reports | `smaragd/tw303a/plugins/src/twpluginslotproc.cc:968` |
| `render()` holds the processor's `mutex_` for the whole call | `twpluginslotproc.cc:1027`; `twpluginslotproc.h:392` |
| A rebuild re-instantiates every instance from the factory | `twpluginslotproc.cc:392-430`; `setFactory`, `twpluginslotproc.h:154` |
| The app reaches the live instance(s) only by raw pointer, outside `mutex_` | `twpluginslotproc.h:262-263` |
| The slot builds its factory from the registry, one fresh instance per call | `spluginslot.cpp:112-119`; `spluginslot.h:271` |
| "Stale this slot and everything above it" is one call | `spluginslot.h:170` (`notifyPluginEdited`) |
| A slot already serializes `SLink` children (none today) | `spluginslot.cpp:391-395` |
| The generator rows: 0→2 is `DirectGen` on 2 channels, `GenFold` on 1, **Unsupported** on more than 2 | `plugins/CONTRACT.md:255-262` |
| A non-contiguous generator page is reset + chase + pre-roll | `plugins/CONTRACT.md:570-594` (inv. 40) |
| The tempo reaches a plugin per chunk in `twProcessContext` | `twpluginslotproc.cc:955-962` |
| The run barrier: forget continuity, then invalidate to the end, main thread | `docs/contracts/FREEZE_PROTOCOL.md:129-137`; `smaragd/main/shell/src/sapplication.cpp:1139-1161` |
| An offline render starts the barrier before the session thread | `sapplication.cpp:1194-1206` |
| The `render` verb waits for a render by **sleeping on the main thread** | `smaragd/main/actions/src/srenderaction.cpp:143-151` |
| `arrangementChanged` fires after every applied action | `smaragd/main/model/include/app/model/sproject.h:280-284` |
| Per-component content epoch, lock-free | `smaragd/tw303a/graph/include/tw/graph/twcomponent.h:483-492` |
| An asset is a registered body; placements link the body itself | `sproject.h:230-247` |
| Object ids are pointer values, rewritten on every save; links resolve by id within one file | `smaragd/main/model/src/sobject.cpp:133`; `smaragd/main/persistence/src/sprojectloader.cpp:154-164` |
| There is no verb that renames an asset | `docs/ACTIONS.md` (no `rename-asset`; asset verbs at :229, :288, :299) |
| The cycle guard for placements | `smaragd/main/model/include/app/model/sarrangements.h:62-77` |
| A freeze cycle answers silence | `docs/contracts/FREEZE_PROTOCOL.md:38-40` |
| A track has ONE input: audio, MIDI or keyboard | `docs/ACTIONS.md:360` (`set-track-input`); `smaragd/main/shell/src/saudiorecorder.cpp:65-70` |
| The recorder submits `place-recording` per armed lane at stop | `saudiorecorder.cpp:893-933` |
| The record planner is a LANE planner behind `PassVerbs` | `smaragd/main/objects/recplan/CONTRACT.md:1-7`, `:46-60` |
| A growing take is not reproducible; stop hands over with one copy | `smaragd/tw303a/sources/CONTRACT.md:159-162`, `:168-172` |
| The live lane: device block, fallback 1024 | `smaragd/main/shell/src/slivemonitor.cpp:705-706` |
| A lane exists iff armed ∪ monitor ∪ metronome | `smaragd/main/shell/CONTRACT.md:812-813` |
| The live lane refuses device rate ≠ project rate | `smaragd/tw303a/playback/CONTRACT.md:245-252` |
| The live event source has exactly one reader | `smaragd/tw303a/devices/include/tw/devices/twliveeventsource.h:72-77`; `plugins/CONTRACT.md:977-979` |
| …and the MIDI input already fans out to up to 8 private sinks | `smaragd/tw303a/devices/include/tw/devices/midi_in_fanout.h:39-66` |
| The vocoder takes onset keyframes; it is bit-exact under partition | `smaragd/tw303a/sources/include/tw/sources/twpagedvocoder.h:110-112`, `:115-121` |
| The vocoder is never on the RT path | `sources/CONTRACT.md:23-31`, `:80-83` |
| `tw_plugins` may depend on core, graph, events only | `tools/check_layering.py:48` |
| `objects/midi`'s rank and edges | `tools/check_layering.py:142`; `smaragd/main/objects/midi/CONTRACT.md:13-17` |
| The CLAP submodule's optional build, and the sentinel `ensure_submodules` checks | `smaragd/tw303a/CMakeLists.txt:239-294`; `_env.sh:426-427` |
| CI checks out submodules recursively in the build job | `.github/workflows/ci.yml:37-43` |

---

## 3. Decisions

### 3.1 Already decided by the author (DESIGN.md, 2026-10-26)

These are inputs. This proposal does not reopen them; where qbx makes one
costlier than DESIGN.md says, §10 and §9 say so.

| # | Decision | DESIGN.md |
|---|---|---|
| A1 | A dependency-free core in nassau-digital; the first front end native in qbx, as a `tw`-format instrument | "Where it lives" |
| A2 | ~~The core is a git submodule of qbx~~ — **superseded 2026-10-26 by the author (Q-3, answered):** the core is pinned **once, as a top-level nassau-suite submodule**, like nassau-plugin-sdk; qbx finds it through a locator (explicit `NASSAU_DIGITAL_DIR`, sibling `../nassau-digital`, pinned fetch) and carries **no** nested submodule | §9.2, as amended; §11 |
| A3 | Programs are model data, not a state blob: the slot's state holds a **reference**, the program lives in the model | §9.1, §9.2 |
| A4 | **One XML grammar, owned by the core**; qbx hands it the element subtree as text | §9.2, §12 |
| A5 | `.qxa` verbs with the same syntax; the command field takes the XML itself, no shorthand | §9.3 |
| A6 | Addressing by `trackPath` + slot, as the plugin verbs do, and a zone or layer by its stable id; `zone="all"` for the ALL scope | §9.3, §13 "settled" |
| A7 | Generations: installing one is a state change; renders pin them at the run barrier and an offline render waits there; "next note" is the live lane's rule only | §6.1 |
| A8 | Live asset links **from the start**, re-rendered only when that asset's own content epoch moved, debounced; self-reference refused | §6, §6.2 |
| A9 | Tempo modes by **stretch** (qbx's `twPagedVocoder`) **and** resample, both from the start; stretch is rendered ahead into a new generation, again on a tempo change and on onset-analysis completion | §5 |
| A10 | Recording goes through qbx's recording; `place-sampler-recording` lands the take in the instrument; playable at stop; Pad/Key landing rules | §7 |
| A11 | Views Key (default), Pad, Map, Zone; a per-user default; the instance's view is saved with it | §7 |
| A12 | Hand selection reads its own tap of the device input, not the engine's event feed | §7 |
| A13 | The live lane is the live path, under its conditions (armed or monitoring, device rate = project rate, device block) | §9.4 |
| A14 | Main + 8 stereo aux outputs; an unrouted aux falls back to main; return tracks and sidechain inputs are **a qbx proposal of their own, built first** — proposal 55 | §3.1 |
| A15 | Round-robin by note position, stateless; a strict cycle only if that does not satisfy | §2.2 |
| A16 | Polyphony 8…256 in powers of two, default 32, allocated at prepare | §4.5 |
| A17 | Stereo out, deterministic like the 303 | §9.3 |

### 3.2 Proposed here

| # | Proposal | Why |
|---|---|---|
| B1 | The core is **required**: when the locator finds it nowhere (no explicit dir, no sibling, fetch off or failing), configure fails with the command that fixes it, instead of degrading like CLAP | A sampler that silently vanishes from a build would take its `.qxa` cases with it and pass CI testing less than it claims, which is exactly what `ci.yml:39-42` warns about for CLAP |
| B2 | `tw/sampler` registers the instrument through a **new explicit registration call** into `twPluginRegistry`, made by the composition root (`SApplication`) and by tests; never a static initializer | The registry cannot name a module that depends on it (§1); inv. 1 forbids static-init self-registration, not an explicit call |
| B3 | Every mutation of a live sampler instance goes through **one new processor entry point that runs under `mutex_`** and bumps the param epoch, plus an **instance initializer** the slot applies to every instance its factory makes | The only reach the app has today is a raw pointer outside the lock (`twpluginslotproc.h:262-263`), and a rebuild discards every instance (`twpluginslotproc.cc:392-394`), so a snapshot pushed once would be lost on the next channel-count change |
| B4 | The program model object is an `SObject` child of the `SPluginSlot`, serialized through the slot's existing `SLink`-children loop; an asset-backed layer is an `SLink` to the asset body | A3 and DESIGN §9.2's "an ordinary link to the asset"; the loader resolves `SLink` by object id within the file, so a link survives a rename where a name would not |
| B5 | ~~`objects/sampler` depends on `objects/track` (for `SPluginSlot`)~~ — **superseded 2026-10-27 by the author (Q-6, answered):** `objects/sampler` keeps the rank of `objects/midi` and has **no** edge to `objects/track`; the **shell** installs the slot's instance initializer as a hook (see *Layering* below). Nothing below the UI depends on `objects/sampler`; a new UI slice `samplerui` holds the editor | §4.4; DESIGN's "rank of objects/midi" cannot hold as stated, see Q-6 |
| B6 | The verbs take `slotIndex`, the attribute `set-plugin-param` already uses (`docs/ACTIONS.md:353`), not DESIGN's `slot` — **confirmed 2026-10-27 (Q-5, answered)**; DESIGN §9.3 and GATES Q1.1 now say `slotIndex` (nassau-digital#15) | A6 says "the way the plugin verbs already do"; see Q-5 |
| B7 | The sampler reports **one** output bus until Q8, and its aux buses only once proposal 55's host passes every bus | The host hands a generator one bus (`twpluginslotproc.cc:968`); a plugin that reports nine and writes `outBuses[1]` reads past the array |
| B8 | The offline-render wait at the barrier is a **synchronous settle on the main thread**, bounded and logged: wait for each outstanding generation job, install its result, then start | The `render` verb sleeps on the main thread (`srenderaction.cpp:143-151`), so a wait that needed the event loop to install a generation would deadlock every scripted render |
| B9 | An asset is rendered to a sample by a bounce of its root component on a worker, through the page scheduler, into memory — the `SFeelFlowTrackBounce` precedent (`smaragd/main/objects/track/src/sfeelflowbounce.cpp:200`) with a memory writer that lives in `tw/render` | The app may not include `tw/sinks` (`smaragd/tw303a/render/include/tw/render/render_session.h:31-36`), so the writer cannot live in the app |
| B10 | The round-robin "same page" of DESIGN §2.2 is the qbx page grid, `twOutputPage::FRAME_CAPACITY` frames from frame 0, read from `twProcessContext::position` | The core sees 4096-frame chunks (`twpluginslotproc.h` `kChunkFrames`), never a page; the cell has to be a constant both sides agree on |

---

## 4. Design

### 4.1 Finding the core, the build and CI

**One pin, in nassau-suite (A2, the author's decision of 2026-10-26).** A
Nassau repo that other Nassau repos consume is pinned **once**, as a top-level
submodule of nassau-suite, exactly as nassau-plugin-sdk is, and never as a
nested submodule of a consumer: a second pin is a second answer to "which core
shipped?", and nassau-suite's `ci/release.sh` reads only its own `.gitmodules`,
so a nested pin would also go untagged. nassau-digital and the shared
`nassau-dsp` it uses from core gate C5 on are therefore both suite submodules
(nassau-dsp joins in nassau-suite#33; nassau-digital joins with Q0). qbx has
**no** `.gitmodules` entry for either.

**The locator.** `smaragd/cmake/NassauDigital.cmake` is a verbatim copy of a
canonical locator kept in nassau-digital (`cmake/bootstrap/`, as nassau-dsp
keeps `cmake/bootstrap/NassauDSP.cmake` and nassau-plugin-sdk keeps
`NassauSDK.cmake`). It resolves, in one order:

1. an explicit `-DNASSAU_DIGITAL_DIR=` — what the nassau-suite superbuild
   passes, so a release builds exactly the pinned core;
2. a sibling checkout `../nassau-digital` beside the qbx checkout (the
   workspace layout, and what an IDE opened on the workspace sees);
   *(Q0: "beside the checkout" needed a variable — §11.1.)*
3. a FetchContent of `NASSAU_DIGITAL_GIT_REF`, a **commit or tag, never a
   branch**, pinned in `smaragd/tw303a/CMakeLists.txt`, so a bare clone of qbx
   and qbx's own CI still build. The repo is public; no token.

Every configure logs the route taken and the commit built ("report the
outcome, not the option"). Inside the core, nassau-dsp is found the same way
by the core's own `cmake/NassauDSP.cmake`; the suite passes
`-DNASSAU_DSP_DIR` to qbx as well, so the core and nassau-analogue are built
against the one pinned nassau-dsp.

**CMake.** The core builds as a plain static library from its own
`CMakeLists.txt`, added from `smaragd/tw303a/CMakeLists.txt` with
`add_subdirectory(${NASSAU_DIGITAL_DIR} nassau-digital EXCLUDE_FROM_ALL)` once
the locator has resolved it. The core's own test and CI targets are not built
inside qbx (the core adds them only as the top-level project). If the locator
finds nothing, configure stops with `FATAL_ERROR` naming the clone command
(B1) — the opposite of the CLAP block's `WARNING`
(`tw303a/CMakeLists.txt:294`), on purpose.

The core target is linked **PRIVATE** into `tw_sampler` only, the discipline
the CLAP block states for its headers (`tw303a/CMakeLists.txt:234-237`): no
other module, and no app module, may see a core header. Two consequences:
`check_layering.py` needs no rule for the core's include paths, because only
`tw/sampler` can compile them; and the app talks to the core only through
`tw/sampler`'s public headers.

Compile flags: the core is built with qbx's flags, and R7 forbids
`-ffast-math` and `-march=native`; qbx sets neither (checked:
`smaragd/CMakeLists.txt`, `tw303a/CMakeLists.txt`, `main/CMakeLists.txt`).
A future qbx flag change that adds either would break R7 silently for the core,
so `tw_sampler`'s block asserts their absence at configure time.

**`_env.sh` and CI.** Nothing changes in `ensure_submodules`
(`_env.sh:426-427`): there is no new submodule. qbx's CI takes route 3, the
pinned fetch, so the build job needs network access to github.com, which it
already has for its checkout; nassau-digital and nassau-dsp are public. The
static job is unaffected: `check_layering.py` walks only
`smaragd/tw303a/<module>` and `smaragd/main/<module>`.

**A core bump is its own qbx PR**, `chore`-sized: it moves
`NASSAU_DIGITAL_GIT_REF`, and is gated by the full suite. A bump that changes
sound changes the sampler's goldens, and the PR says which and why; a bump that
changes no sampler golden is evidence it changed no sound. For a release, the
**suite's** pin is the authority, and a suite pin bump of nassau-digital is the
release-side counterpart.

**nassau-suite.** The suite adds `nassau-digital` to its `.gitmodules` with
Q0 and passes `-DNASSAU_DIGITAL_DIR` and `-DNASSAU_DSP_DIR` to qbx's
ExternalProject. `ci/release.sh` then tags nassau-digital and nassau-dsp at the
commits the suite pins, with no change to the script (Q-3, answered). One
honest consequence: qbx's own `NASSAU_DIGITAL_GIT_REF` (what a bare clone
builds) and the suite's pin are two different numbers, as qbx's and the
suite's SDK refs are today; the suite build never uses qbx's, and the
configure log says which commit was built.

### 4.2 `tw/sampler` — the engine module

```
tw/sampler   deps: core, graph, events, plugins, sources   (+ the core, PRIVATE)
```

* **`plugins`**: `twSamplerInstrument` is a `twPlugin`. **`sources`**: the
  stretch renderer runs `twPagedVocoder` (§4.8). **`events`**: arrives anyway
  through `plugins`, named for clarity. No `pages`, no `mix`, no `playback`.
  Nothing depends on `tw/sampler` except the app layer and its tests, so the DAG
  stays acyclic.
* `check_layering.py`: `DEPS['sampler'] = ['core', 'graph', 'events',
  'plugins', 'sources']`, with a comment in the house style naming this
  proposal. The `TW_INCLUDE` pattern (`check_layering.py:92`) already matches
  `tw/sampler/…`.
* `tw303a/CMakeLists.txt`: `tw_module(sampler …)`, the core linked PRIVATE,
  `tw_sampler` added to the umbrella `tw303a` target (`:1042-1043`), and a
  `tw_module_test(sampler_test …)`.

**Public headers** (`tw/sampler/…`), all Qt-free (THREADING.md rule 1):

* `twsamplerhost.h` — the extension interface of DESIGN §9.1, reached by
  `dynamic_cast<twSamplerHost *>( twPlugin * )`. There is no generic query in
  `twPlugin`, and adding one for a single consumer would widen the narrow
  interface for nothing; a cast is the honest spelling of "capability-queried"
  until a second extension exists.

  ```cpp
  class twSamplerHost {
  public:
      virtual void setProgram( std::shared_ptr<const twSamplerProgram> ) = 0;
      virtual void setSample( std::uint32_t layerId,
                              std::shared_ptr<const twSamplerSample> ) = 0;
      virtual void setRoutedOutputs( std::uint32_t mask ) = 0;   // bit k = aux k+1
      virtual void collectRetired() = 0;   // drop retired snapshots; NOT the audio thread
      // (Q0: returns int, and a fifth, stats(), was added — §11.3.)
  protected:
      ~twSamplerHost() = default;
  };
  ```

  All four are called only from inside the processor's new locked entry point
  (§4.3), so `process()` reads the snapshots without a lock of its own (R3).
  `twSamplerSample` is DESIGN §2's immutable PCM: planar float, 1 or 2
  channels, its own sample rate, frames, and its **generation**.
* `twsamplerregistry.h` — `void registerNassauDigital( twPluginRegistry & )`,
  the B2 entry point.
* `twsamplerstretch.h` — `renderStretched( const twSamplerSample &src, double
  stretch, const uint64_t *onsets, size_t n ) → shared_ptr<const
  twSamplerSample>`, worker threads only.

**The descriptor.** `format="tw"`, `uid="tw.native.nassaudigital"` (GATES Q0.1),
`io = {0, 2}`, `isInstrument`, `acceptsNotes`, one event port in, no event out,
`nOutBuses = 1`, `outBusChannels = {2}` until Q8 (B7). The display name is a
product string and comes from the core or a constant in `tw/sampler`, never a
`"Smaragd"` literal (`tools/check_product_name.py` job 1).

**The registration seam (B2).** `twPluginRegistry` gains
`registerBuiltin( const twPluginDescriptor &, std::function<std::unique_ptr<twPlugin>()> )`.
The pair is kept in a list of its own; `appendBuiltins_nolock()`
(`twpluginregistry.cc:292`) appends those descriptors after the 303 on every
seed and every rescan, and `instantiate()` consults the list before the format
branches (`:704-709`). `SApplication` calls `audio::registerNassauDigital(
audio::pluginRegistry() )` beside its existing registry configuration
(`sapplication.cpp:693`), before the first scan; `sampler_test` calls it
itself. It is symbol-referenced (inv. 1 holds, and `plugins/CONTRACT.md`
records the new seam under it). The probe executable needs nothing: built-ins
are never scanned.

**The state blob (A3).** `saveState()` writes an 8-byte header (`'TWND'`, u16
version, u16 reserved, as the CLAP frame does, inv. 3) and the program-level
parameters (§4.5); never the program, never PCM. *(Q0: no program-level
parameter exists until Q-2 is answered, so the blob is the header alone —
§11.5.)* `loadState()` refuses a newer
version (inv. 3). The program comes from the model through `setProgram`.

**Buses and width.** The instrument always renders stereo (A17). On a
2-channel project the slot takes `DirectGen`, on a 1-channel one `GenFold`
(`plugins/CONTRACT.md:255-258`). On a project wider than 2 channels it is
**Unsupported and transparent**, like every stereo instrument
(`plugins/CONTRACT.md:262`, gated by `qxa.instrument_stereo_render`'s refused
row). DESIGN §9.3 says qbx "spreads them on a wider one"; it does not (Q-7).

**Round-robin (A15, B10).** The core receives the absolute project frame of
each chunk in `twProcessContext::position` (set with `twCtxPosition` on every
freeze, `twpluginslotproc.cc:947-953`). The "same page" of DESIGN §2.2 is the
cell `floor(position / twOutputPage::FRAME_CAPACITY)`; `tw/sampler` passes the
cell size to the core at prepare, so the core never names a qbx type. *(Wrong
against the core as built — it takes no cell size; `tw/sampler` calls
`nd::Instrument::startPage()` on every cell change instead. §11.2.)* Because a
page render always starts at a cell boundary and the pre-roll replays the notes
before it (inv. 40), a page rendered alone and the same page rendered after its
predecessor see the same notes of their cell — the property C3.4 tests in the
core and Q2 re-tests through qbx's real page renders.

**Buffers are never released on the audio path (R3).** A voice keeps the
generation it started with (A7). When `setSample` replaces a buffer a voice
still reads, the instance moves the old `shared_ptr` to a fixed-size retire
list instead of dropping it; `collectRetired()` releases what no voice uses.
*(Q0: the core already parks replaced buffers itself, so `tw/sampler` keeps no
retire list of its own — §11.4.)*
The app calls it from the main thread after every install and from a slow
timer while the slot exists. `process()` never holds the last reference.

### 4.3 The slot seam (`tw/plugins` + `objects/track`)

Two additions, both generic — neither names the sampler:

* `twPluginSlotProcessor::applyToInstances( const std::function<void( twPlugin & )> & )`:
  takes `mutex_`, calls the function on every instance (dual-mono has N,
  `twpluginslotproc.h:262-263`), and calls `bumpParamEpoch_nolock()`. Because
  `render()` holds the same mutex for the whole call (`twpluginslotproc.cc:1027`),
  an install lands **between** two `process()` calls, never inside one, both on
  the frozen path and on the live pump. This is DESIGN §9.1's "swapped under
  the slot processor's mutex".
* `SPluginSlot::setInstanceInitializer( std::function<void( audio::twPlugin & )> )`:
  `makeFactory()` (`spluginslot.cpp:112-119`) wraps the registry call so every
  fresh instance is passed through the initializer before the processor sees
  it. That is what keeps the current program and every current generation
  across `setChannelCount()`, `reloadPlugin()` and a rescan, which all rebuild
  (`twpluginslotproc.cc:392-394`).

An install is then: `applyToInstances(install)` → `notifyPluginEdited()`
(`spluginslot.h:170`), which stales the slot's pages **and** the track's
render path through `audioInvalidated()`; the slot cannot do the second half
itself (`spluginslot.h:240-250`).

### 4.4 `objects/sampler` — the app slice

**Model.** `SSamplerProgram : SObject` wraps one core program, Qt-free inside
(A3: one model). It holds the core's program as the truth and hands
`shared_ptr<const twSamplerProgram>` snapshots to the instance; it owns undo,
links and signals. It is an `SObject` child of the `SPluginSlot` (B4), created
when a NassauDigital slot is inserted and when one is loaded, and installed as
the slot's instance initializer. *(Q1: created by the first edit that succeeds,
not at insert — §11 Q1 item 2.)*

**Persistence (A4).** `SSamplerProgram::serialize` writes its own element with
the core's program element **nested verbatim** inside it, followed by one
`SLink` child per asset-backed layer, each naming the layer id it serves. On
load the loader hands the element over; qbx serializes the nested
`<NassauDigitalProgram>` subtree back to text and gives the core that string.
*(Wrong as built: QDom reorders attributes, so the core's element is stored as
CDATA text — §11 Q1 item 3.)*
qbx never reads a program attribute itself, so unknown attributes and elements
survive by the core's rule (R4, C1.2). It self-registers with `SProjectLoader`
from a static initializer, as `SPluginSlot` does
(`smaragd/main/objects/track/CONTRACT.md:193-195`) — legal in an OBJECT
library (`docs/ARCHITECTURE.md`, "OBJECT is load-bearing").

**Layer sources (DESIGN §6).** A layer's `src` in the core grammar is an opaque
string the core never interprets. qbx writes:

| Source | `src` | Resolution |
|---|---|---|
| A file | `file:<portable path>` | the project's extern-file machinery (`app/model/sfilepathref.h`), so relocation and "collect files" treat it like any sample |
| A recording made in the instrument | `file:<path under the project's recordings folder>` | as a file; see Q-4 |
| A qbx asset | `link:<layer id>` | the `SLink` child carrying that layer id, targeting the asset body (B4) |

**Layering (B5) — superseded by the Q-6 answer, 2026-10-27.** The author
kept DESIGN §9.2's rank: `objects/sampler` sits at the rank of
`objects/midi` and does **not** depend on `objects/track`. The block below is
the shape this proposal first argued for and is kept for its reasoning; the
built shape differs in one edge:

* `APP_DEPS['objects/sampler']` drops `objects/track`. The engine edges stay
  (`plugins` still names `twPlugin &` in the initializer the slice provides).
* The **shell**, which may already see both slices, wires the hook: it hands
  the slot an instance initializer that `objects/sampler` provides, so the
  slot calls it on every instance it creates without either slice naming the
  other. Q1 records the exact seam as built, here and in the three CONTRACTs.
  *(Built in Q1: two hooks and a generic slot seam — §11 Q1 item 1.)*

As first proposed:

```
APP_DEPS['objects/sampler'] = {'actions', 'model', 'objects/track', 'persistence'}
APP_ENG ['objects/sampler'] = _ENG_BASE | {'events', 'pages', 'plugins',
                                           'render', 'sampler', 'schedule',
                                           'sidecar', 'sources'}
```

`objects/track` because the slot it configures lives there; `plugins` for
`twPlugin &` in the initializer; `sampler` for `twSamplerHost`; `sources` to
read a file layer through `SObject::getRandomSource()`; `render` + `schedule`
+ `pages` for the asset bounce (B9); `sidecar` to read a source's `onsets`
aspect (§4.8); `events` for the tempo map (beats → frames). It is **not**
added to `objects/track`'s, `objects/midi`'s or `objects/mixer`'s edges: the
track keeps knowing nothing of the sampler, which is the property DESIGN §9.2's
"rank of objects/midi" exists to protect. It is added to `shell` (the run
barrier, the recorder), `samplerui` and `testkit`.

`main/CMakeLists.txt` lists sources explicitly — a new `.cpp` not listed there
is silently not compiled and the build still succeeds — so every milestone adds
its files there in the same change; `app_objects` gains `tw_sampler` beside
`tw_mix tw_plugins tw_sidecar` (`main/CMakeLists.txt:685`).

**`samplerui`** (app_ui, rank of `eventui`/`pluginui`):
`APP_DEPS = {'actions', 'model', 'objects/sampler', 'objects/track',
'pluginui', 'servicesui', 'shell'}`, `APP_ENG = _ENG_BASE | {'devices',
'events', 'plugins'}` — `devices` for its `MidiInFanout` tap (A12),
`servicesui` for the per-user default view's key name in `SOpt` (the reason
`eventui` and `mediabrowser` have that edge). How the FX strip opens it is in
§4.11.

### 4.5 The verbs

Addressed by `trackPath` + `slotIndex` (B6) + a stable `zone` / `layer` id
(A6). `zone="all"` is the ALL scope; `zones="3;7;12"` the selected set. A
leading `+` / `-` on a numeric value is a relative edit (DESIGN §2.1); on an
enum it is **refused**. Every inverse restores **absolute** previous values,
per zone (C1.5). Every refusal is announced with a `TW_LOG` line and mutates
nothing.

| Verb | Milestone | Attributes (beyond the address) | Inverse |
|---|---|---|---|
| `add-zone` | Q1 | the zone's attributes; `id` is assigned from `nextId`, or named explicitly when free and `>= nextId` (QBX-265, the author's answer of 2026-11-01) | `remove-zone` with the full zone |
| `remove-zone` | Q1 | `zone` | `add-zone` restoring the id and every layer |
| `set-zone` | Q1 | `zone` \| `zones` \| `zone="all"`, any zone attribute | `set-zone` per zone, absolute |
| `add-layer` / `remove-layer` | Q1 | `zone`, layer attributes / `layer` | each other |
| `set-layer` | Q1 | `layer`, any layer attribute, `src` (§4.4) | absolute |
| `set-sampler-program` | Q1 | `polyphony`, `level`, `tune`, `busDrive`, … | absolute |
| `select-zones` | Q1 | `zones`, `mode` = `replace`\|`add`\|`toggle` | the previous selection |
| `set-sampler-view` | Q7 | `view` = `key`\|`pad`\|`map`\|`zone`, `padBank` | absolute |
| `set-sampler-routing` | Q8 | `mask` (DESIGN §3.1) | absolute |
| `place-sampler-recording` | Q6 | `filePath`, `srcOffset`, `length`, `target` = `pad`\|`key` (resolved by the recorder from the view, §4.10), `key` | `remove-layer` / `remove-zone`, whichever it created, or the replaced layer 1 |

`select-zones` is an action because GATES Q1.1 asks that it be undone by one
undo; it changes no sound and bumps nothing.

Test verbs (testkit): `assert-sampler-program` (structural equality against a
program file or inline XML, and named fields), `assert-sampler-layer`
(`generation`, `rendering` mark, `renders` counter for Q3.2),
`set-sampler-render-gate` (holds a layer's next generation job, the
`set-render-gate` pattern, `smaragd/main/testkit/src/ssidecartestactions.cpp:299`),
and `sampler-hold-keys` (injects held keys into the editor's tap, Q7.2).

Every one gets a row in `docs/ACTIONS.md` in the milestone that adds it, and
the round-trip table in `smaragd/main/testkit/src/action_roundtrip_test.cpp`
(as `place-recording` has, `:352-353`).

### 4.6 Generations, and what a render hears (DESIGN §6.1)

**Install.** Each layer has a generation counter in `SSamplerProgram`. A new
buffer — from a file load, a recording, an asset bounce, a stretch — is
computed on a worker (never Qt), posted to the main thread, and installed by
§4.3's sequence. Installing is a state change (A7): the param epoch moves, the
slot and the track's render path are staled, every page is re-rendered against
one generation.

**The barrier, offline (B8).** `SApplication::beginRun()` already walks every
instrument track on the main thread before any demand
(`sapplication.cpp:1139-1161`). For each NassauDigital slot it now first calls
`ssampler::settleBeforeRun( slot )`, which — **only for an offline render** —
waits on each outstanding generation job of that program (bounded, by default
30 s, overridable like `SMARAGD_RENDER_TIMEOUT_MS`), installs the results
synchronously, and logs

    run barrier: waited <ms> ms for <n> sampler generation(s) on track <path>

which is the line GATES Q2.3 asserts. *(Built: `startRender()` calls
`ssamplerwiring::settleBeforeRender()` just before `beginRun()`, which cannot
refuse — §11 Q2 item 1.)* Then the existing
`forgetContinuity()` + `invalidateRenderPathRange()` runs, in that order. The
walk-not-registry rule of the barrier (`objects/track/CONTRACT.md` inv. 14)
holds: the settle finds its slots through the same walk.

A job that cannot finish inside the bound fails the render with a reason
(`startRender`'s `error`, QBX-145's discipline), never a render of the older
generation pretending to be current.

**Pinned during a render.** While `SAppContext::isRenderingActive()`, an
arriving generation is **queued**, not installed, and the queue is flushed on
the main thread when the render completes. An install mid-render would stale
pages under a running render and give it two generations. *(Built, and not
gated: no scripted render can meet an arrival — §11 Q2 item 7.)*

**Playback start** runs the barrier without the settle: a playback run renders
whatever generation is installed, and a later install invalidates as any edit
does. DESIGN §6.1 asks byte-identity of renders, not of playback.

**Live (rule 4).** On the live lane a voice reads the generation current at its
note-on and keeps it (A7); with §4.3 an install lands between two pump blocks,
so "current at note-on" is well defined.

### 4.7 Live asset links (DESIGN §6.2)

* **Trigger.** `SSamplerProgram` listens to `arrangementChanged`
  (`sproject.h:280-284`) and, for each asset-backed layer, compares the asset
  root component's `contentEpochNow()` (`twcomponent.h:490`) with the epoch its
  current generation was bounced from. Only a moved epoch schedules a bounce,
  debounced (the interval is a measured choice, recorded in the CONTRACT), so
  an unrelated edit costs one integer compare per linked layer.
* **The bounce (B9).** A worker bounces the asset body's root component over
  its extent through the page scheduler, as `SFeelFlowTrackBounce` bounces a
  track (`sfeelflowbounce.cpp:200`): a `RenderSession` with a scheduler, a
  prune scope limited to the asset's components (`render_session.h:102`), no
  `onPosition` (a bounce must not move the locator), and a memory writer from
  `tw/render` via `setWriterFactory` (`render_session.h:112`). The result is a
  `twSamplerSample` at the project rate with the next generation number. A
  render counter per layer (`assert-sampler-layer renders=`) is what Q3.2 reads.
* **While it runs**, the layer is marked rendering (DESIGN §6.2) and the old
  generation keeps playing.
* **Self-reference is refused when the link is made.** `set-layer
  src="link:…"` refuses when the asset body reaches the sampler's own track,
  `sarrangements::reaches( body, track )` (`sarrangements.h:62-67`). That walk
  follows `childLinks()` and a plugin chain is deliberately not a child link,
  so it cannot see a cycle that runs **through another sampler** (A's layer
  links asset X, X contains track B, B's sampler links asset Y, Y contains A).
  The refusal therefore also walks sampler links: for every NassauDigital slot
  on a track reached from the body, its asset links are followed too. The
  freeze-cycle silence (`FREEZE_PROTOCOL.md:38-40`) remains the backstop, not
  the guard.
* **Rename.** The link is an `SLink` to the body, resolved by object id inside
  the file (B4), so a rename changes nothing the link holds. But there is no
  rename verb today (§2), and GATES Q3.4 needs one; Q3 adds `rename-asset`
  (`assetName`, `newName`; refuses a name in use; inverse is itself) as a
  generic verb in `objects/mixer`, beside `create-asset`.

### 4.8 Tempo modes (DESIGN §5)

* **resample** is a pitch ratio in the voice (C7): qbx's only part is the tempo
  in `twProcessContext`, which already arrives per chunk
  (`twpluginslotproc.cc:955-962`). No Q gate of its own; Q4's cases cover it in
  passing.
* **stretch** is rendered ahead. For a layer in a tempo mode, the stretch
  factor is `(beats · 60 / bpm) / (frames / rate)`. A worker runs
  `twsamplerstretch` (the vocoder, `twpagedvocoder.h:110-112`, bit-exact under
  partition, `:115-121`) and the result is installed as a new generation (§4.6).
  A new generation is rendered when (a) `set-tempo` changes the tempo — the
  slot already receives the tempo map and bumps on a change
  (`twpluginslotproc.cc:218-226`) — and (b) the source's `onsets` aspect lands.
  The onsets are read from the sidecar store by the source's content hash; with
  none yet, the first generation uses the vocoder's fixed keyframe grid, and the
  analysis completion produces the second (A9). For a file layer the analysis
  is the one `SPlainWave` already enqueues; for a bounced asset or a recording
  `objects/sampler` enqueues the same analyzer on the bounced buffer.
* Both renders feed §4.6, so an offline render after a tempo change waits for
  the stretch at the barrier.

### 4.9 Live play (DESIGN §9.4)

Nothing new in the live lane. The sampler is a generator in slot 0, so it takes
the path every instrument takes: the track armed or monitoring
(`shell/CONTRACT.md:812-813`), the live event source on the pump
(`plugins/CONTRACT.md` inv. 46), the device's block size
(`slivemonitor.cpp:705-706`), and the rate refusal
(`playback/CONTRACT.md:245-252`) when device and project rates differ. The
sampler's own obligations are R3 and a per-block cost small enough not to raise
the latency, which C2.6 budgets. Q5 measures it, headless against the live
lane's bound (as `live_instrument_play.qxa` does, with `midi-in-event` and
`assert-audio-onset afterMidiIn="1"`), and by hand against nassau-analogue.

### 4.10 Recording into the instrument (DESIGN §7)

**Not a `PassVerbs` implementation.** `PassVerbs` is the seam a LANE planner
emits through (`recplan/CONTRACT.md:46-60`); a planner reads a lane snapshot
and lands material among a lane's children. A take that lands in an instrument
has no lane to plan against, so `place-sampler-recording` is a sibling of
`place-recording`, submitted by the recorder at stop, not a planner.

**The flow.** The capture is qbx's: input monitoring, latency compensation and
the capture file are the recorder's. At stop the recorder, for an armed track
whose recording targets a NassauDigital instance, submits
`place-sampler-recording` instead of `place-recording`, inside the same commit
scope (`saudiorecorder.cpp:930`). The verb reads the capture file (the growing
source is not reproducible, `sources/CONTRACT.md:159-162`, so the sample exists
only from stop), creates or replaces the zone / layer by DESIGN §7's rule, and
installs the first generation. The landing rule needs the view and the last key
played; both are model state of `SSamplerProgram` (the last key from the
editor's tap, §4.11), so the verb is deterministic given its attributes.

**Which input.** A track has one input: audio, MIDI or keyboard
(`docs/ACTIONS.md:360`; the audio recorder skips a MIDI-input track,
`saudiorecorder.cpp:65-70`). Playing a sampler live needs a MIDI input on its
track; recording into it needs an audio input. How one instrument gets both is
not something DESIGN.md says (Q-1); until it is answered the milestone can be
built and gated with the recording made on the instrument's own track (audio
input) and played from a sequenced clip.

**Threshold start** (recording begins at the first sound) does not exist in the
recorder (no threshold in `saudiorecorder.cpp` or `tw303a/record`); GATES has
no Q gate for it, so it is out of scope here (§8).

### 4.11 The editor (DESIGN §7, §9.3)

* **Window, not dock.** One editor window per instance, opened from the FX
  strip's edit action. `pluginui` gains a small registry, keyed by uid, of
  "this instrument has an app-side editor"; `samplerui` registers into it from a
  static initializer, and the strip consults it before falling back to the
  native or generic editor. The slot's existing `editorOpen` flag
  (`spluginslot.h:156-157`) persists it, as for native editors.
* **Views** Key (default), Pad, Map, Zone at fixed sizes. The per-user default
  is an `SOpt` key; a switch is `set-sampler-view`, so it is saved with the
  instance (A11).
* **Hand selection (A12)** reads its own `MidiInFanout` sink
  (`midi_in_fanout.h:65`), acquired for the track's MIDI input port while the
  editor is open — a second private ring, so the engine's single-reader live
  source is untouched. Held keys become `select-zones`.
* **The XML command pane (A5)** is an editor with completion of verb and
  attribute names, fed from the verbs' declared attributes; applying is one
  submit, so one undo step. The `trackPath`/`slotIndex` pair is implied inside
  the instance's editor and may be left out.
* **Drag and drop** takes the existing `application/x-smaragd-resource`
  payload (`asset:`, `file:`, `media:`) onto a key, a pad or a zone.
* **Look**: Qt, Smaragd's style (`app/theme`); user-visible text says neither
  Smaragd nor qbx (the product-name rule).

### 4.12 Multiple outputs (DESIGN §3.1)

Q8, after proposal 55 ("return tracks and sidechain inputs") is built. This
proposal designs only the sampler's side: from Q8 the instrument reports nine
output buses (main + 8 stereo aux, B7), the routed mask comes from whatever
proposal 55 makes the model say about which `(track, slot, output)` taps exist,
and `setRoutedOutputs` is called with it through §4.3, as a state change.
`set-sampler-routing` is the verb when the mask is set explicitly. Everything
about return tracks, their caches, their UI and sidechain inputs is proposal
55's.

---

## 5. Invariants

New, each to be written into the CONTRACT.md of the module it belongs to.

1. **THE CORE IS SEEN BY `tw/sampler` ONLY.** Linked PRIVATE; no other engine
   module and no app module includes a core header.
2. **THE INSTRUMENT IS REGISTERED BY CALL, NEVER BY STATIC INITIALIZER.**
   `registerNassauDigital()` from the composition root and from tests
   (`plugins` inv. 1 extended).
3. **EVERY MUTATION OF A LIVE INSTANCE IS `applyToInstances`**, under the
   processor's mutex, with a param-epoch bump, followed by `notifyPluginEdited()`
   on the main thread. No `twSamplerHost` method is ever called through the bare
   `plugin()` pointer.
4. **A FRESH INSTANCE IS BORN CURRENT.** The slot's instance initializer
   installs the current program and every current generation before the
   processor sees the instance.
5. **`process()` NEVER RELEASES A BUFFER.** Retired snapshots are released by
   `collectRetired()` on the main thread.
6. **INSTALLING A GENERATION IS A STATE CHANGE, AND NONE IS INSTALLED DURING
   AN OFFLINE RENDER.** Arrivals during a render are queued and flushed after it.
7. **AN OFFLINE RENDER STARTS ONLY WHEN NO GENERATION IS OUTSTANDING**, by a
   bounded, logged, synchronous settle at the barrier — or it does not start,
   with a reason.
8. **QBX NEVER PARSES THE PROGRAM.** The core's grammar is stored and handed
   over verbatim; qbx reads and writes the program only through the core.
9. **AN ASSET LINK IS AN `SLink` TO THE BODY, AND THE CYCLE REFUSAL WALKS
   SAMPLER LINKS TOO.**
10. **THE INSTRUMENT REPORTS ONLY THE BUSES THE HOST PASSES.** One until Q8.
11. **THE ROUND-ROBIN CELL IS THE PAGE GRID** (`FRAME_CAPACITY` from frame 0),
    given to the core at prepare.
12. **`objects/track` NEVER DEPENDS ON `objects/sampler`.**

---

## 6. Milestones

One milestone per GATES.md Q gate, each one qbx PR with its own ticket, held to
`./ci/gates.sh` (R1) plus the ACs below. **Qn.k** are GATES.md's ACs, carried in
substance; **Qn.qk** are qbx's own. Every milestone also: keeps
`check_layering.py` clean with the new entries commented; updates the CONTRACT.md
of every module it touches; adds every new verb to `docs/ACTIONS.md` and the
round-trip test; watches each new case fail once against a sabotage before
trusting it; and marks the gate DONE in nassau-digital `docs/GATES.md` with the
measured numbers (R10), in a nassau-digital PR of its own.

### Q0 — Engine module and seam
**Depends on:** core gate **C2** merged in nassau-digital (one voice: pitch,
rate fold, play modes, pan law, determinism, per-voice cost), and C0/C1 before
it. Scope: §4.1, §4.2, §4.3.

* **Q0.1** The core is found by the locator (§4.1; never a submodule of qbx); `tw/sampler` builds it and registers
  `tw.native.nassaudigital` through `registerBuiltin`; `check_layering.py` is
  clean.
* **Q0.2** `twSamplerHost` is reached by capability; `setProgram`, `setSample`
  and `setRoutedOutputs` installed from a test through `applyToInstances` are
  read without a lock in `process()` (`sampler_test`, a direct processor test
  in the style of `plugins_test`).
* **Q0.3** A `.qxa` case inserts the instrument with `insert-plugin format="tw"
  uid="tw.native.nassaudigital"`, plays a note at frame 1000, and the render is
  silent before it and carries the sample's RMS after, matching a golden
  (`assert-file-identical`).
* **Q0.4** The slot's saved state holds the reference only: the project file
  grows by less than 1 KB when a 10 MB sample is added.
* **Q0.q1** Each locator route is exercised at configure: an explicit
  `NASSAU_DIGITAL_DIR`, a sibling checkout, and the pinned fetch each log their
  route and commit; with no checkout and the fetch off, configure stops with
  the clone command in the message; an explicit directory holding no core stops
  rather than falling back.
* **Q0.q2** A `setChannelCount` change (a 2 → 1 channel project) keeps the
  program and samples: the render after it is the `GenFold` of the render
  before it (invariant 4).
* **Q0.q3** New `tw/sampler/CONTRACT.md`; `plugins/CONTRACT.md` records
  `registerBuiltin` under inv. 1 and `applyToInstances` beside inv. 15;
  `objects/track/CONTRACT.md` records the instance initializer.
* **Q0.q4** `assert-instrument-slot mode="DirectGen"` on a stereo project and
  `mode="GenFold"` on a mono one.

### Q1 — Model and verbs
**Depends on:** Q0; core C1 (model, XML, ids, ALL scope, absolute inverses).
Scope: §4.4, §4.5 (the Q1 rows).

* **Q1.1** `objects/sampler` wraps the core's program; `add-zone`, `set-zone`,
  `set-layer` and `select-zones` are actions addressed by `trackPath` +
  `slotIndex` + zone id, each undone exactly by one undo (asserted on the
  program with `assert-sampler-program`).
* **Q1.2** The program is saved in the project and reloads identically
  (structural equality); a program element written by a newer version keeps its
  unknown attributes and elements.
* **Q1.q1** A relative edit on ALL clamps per zone and its undo restores each
  zone's previous absolute value; a relative edit on an enum is refused,
  announced, and mutates nothing.
* **Q1.q2** An id is never reused across an undo of `add-zone` followed by a
  new `add-zone`.
* **Q1.q3** New `objects/sampler/CONTRACT.md`; `APP_DEPS`/`APP_ENG` entries;
  the verbs in `docs/ACTIONS.md` and the round-trip test.

### Q2 — Generations and determinism
**Depends on:** Q1. Scope: §4.6, the round-robin cell (§4.2).

* **Q2.1** Installing a generation stales the track's pages: a render after
  swapping a layer's sample hears the new sample from the first frame.
* **Q2.2** Two renders of one session, with a swap between them, are each
  internally made of one generation (no page of the old sample after a page of
  the new).
* **Q2.3** An offline render started while a layer's render is outstanding
  (`set-sampler-render-gate`) waits at the run barrier — its log line is
  asserted — and its bytes equal a render started after the layer was ready.
* **Q2.q1** A generation that arrives during a render is installed after it:
  the render's bytes equal a render with no arrival.
* **Q2.q2** With round-robin in use, a page rendered alone equals the same page
  rendered after its predecessor (C3.4, through qbx's real page renders; needs
  core C3).
* **Q2.q3** A settle that exceeds its bound refuses the render with a reason a
  case can `assert-log`.

### Q3 — Live asset links (decided: from the start)
**Depends on:** Q2. Scope: §4.7.

* **Q3.1** A layer linked to an asset: an edit of that asset produces a new
  generation, and the next render hears the edit.
* **Q3.2** An action that does not touch the asset produces no re-render of it
  (render counter unchanged).
* **Q3.3** Linking an asset that contains the sampler's own track is refused.
* **Q3.4** Renaming the asset leaves the link intact, across a save and a
  reload (with the new `rename-asset` verb).
* **Q3.q1** A cycle through a second sampler (§4.7) is refused.
* **Q3.q2** `rename-asset` has its own case (refuses a name in use; undo
  restores the name and every placement still plays).

### Q4 — Tempo modes by stretching
**Depends on:** Q2, and **the vocoder**: `twPagedVocoder` in `tw/sources`,
which exists; the dependency is the `tw/sampler → sources` edge, and the
onsets aspect from proposal 27 M1. Scope: §4.8.

* **Q4.1** A 2-beat loop at 120 BPM played at 100 BPM by stretch measures a
  period of 2·60/100 s ± 1 ms with its pitch unchanged ± 5 cents.
* **Q4.2** A tempo change produces a new generation, and so does completion of
  the source's onset analysis (`wait-analysis`); renders after each are
  byte-identical across two runs.
* **Q4.q1** An offline render issued straight after `set-tempo` waits for the
  stretch at the barrier (Q2.3's mechanism).

### Q5 — Live play
**Depends on:** Q0 (and the live lane, which exists). Scope: §4.9.

* **Q5.1** Headless: an armed track, an injected live NoteOn (`midi-in-event`);
  the onset lies within the live lane's block-plus-lead bound
  (`assert-audio-onset afterMidiIn="1"`), and the lane reports no ownership
  refusal (`assert-render-policy liveOwnedRefusals="0"`). `RUN_SERIAL`, like
  `live_instrument_play`.
* **Q5.2** **Hand check by the author**, on the Windows 11 machine with the USB
  keyboard: the sampler's live onset, measured the same way as
  nassau-analogue's on the same device and buffer size, is within one device
  block of it. Recorded in GATES.md with both numbers. Not an agent's to
  perform or to claim.
* **Q5.q1** A generation installed while a live voice sounds does not change
  that voice; the next note plays the new one (DESIGN §6.1 rule 4).

### Q6 — Recording into the instrument
**Depends on:** Q1, Q2; and an answer to Q-1 for the dual-input case. Scope:
§4.10.

* **Q6.1** `record-start` → stop → `place-sampler-recording`: the next note
  plays the recording at its RMS ± 2 %; one undo removes it.
* **Q6.2** Where it lands: Pad view → the selected pad's key; Key view on an
  empty program → a zone over the whole keyboard; Key view on a program with
  zones → a one-key zone on the last key played. One case each.
* **Q6.q1** The recorder submits `place-sampler-recording` in the same commit
  scope as any `place-recording` of the same press (one undo for the press).

### Q7 — Editor
**Depends on:** Q1 (Q6 for the recording affordances). Scope: §4.11.

* **Q7.1** Key, Pad, Map and Zone views open at their fixed sizes; the view of
  an instance survives a save and reload; a new instance starts in the per-user
  default view.
* **Q7.2** Holding a key selects its zone, holding two selects both, through a
  tap of the device input (not the engine's event feed); asserted with injected
  MIDI.
* **Q7.3** The XML command pane applies a verb as one undoable action and
  completes verb and attribute names.
* **Q7.4** **Hand check by the author**: every label and value readable in
  every view; a velocity gap shows in the Map view.
* **Q7.q1** New `samplerui/CONTRACT.md`; the editor registry in `pluginui`
  recorded in its CONTRACT; widgets gated off screen through describe/trigger
  seams (the arranger-menu precedent), since no headless gate can look at them.

### Q8 — Multiple outputs in qbx
**Depends on:** **proposal 55 ("return tracks and sidechain inputs") built**,
and core C6. Scope: §4.12.

* **Q8.1** A kick zone on aux 1, aux 1 routed to a return track: the return
  track carries the kick, main does not (goldens of each).
* **Q8.2** A compressor on a bass track keyed from that return track ducks the
  bass while the kick sounds (measured RMS).
* **Q8.3** Unrouting aux 1 sends the kick back to main, and the change is a
  state change: the next render hears it from the first frame.
* **Q8.q1** The descriptor reports nine buses only from this milestone, and the
  host passes all of them (invariant 10).

---

## 7. Risks

* **The core's API does not exist yet.** C1 and C2 are not merged. §4.2 names
  what qbx needs from the core: an immutable program snapshot, edit operations
  returning absolute inverses, parse/write by string, a sample type carrying
  rate and generation, a voice engine taking chunk events, absolute position,
  tempo, routed mask and a round-robin cell size. A mismatch is fixed in
  nassau-digital first, never adapted around in qbx.
* **A bare clone of qbx now needs the network once** to fetch the core at its
  pinned ref (or a sibling checkout). Offline, with neither, configure stops
  with the fix (B1), loudly rather than silently.
* **The settle blocks the main thread** for as long as a stretch or a bounce
  takes. It is bounded and logged; a long asset under a fresh tempo can still
  make an interactive render feel slow to start.
* **Asset bounces cost a full render of the asset** per moved epoch. The
  debounce and the epoch compare bound the frequency, not the size.
* **Memory**: a tempo layer holds a stretched copy per generation in flight;
  retired copies live until `collectRetired()`.
* **The pre-roll.** A sampler voice held across a page start re-renders up to
  4 s of pre-roll per page (inv. 40); long one-shots on a dense map make page
  renders dearer than the 303's.
* **A sibling checkout wins over the pinned ref** for a developer, by design
  (route 2 before 3), so a stale sibling builds stale core code; the configure
  log names the route and commit in use.

---

## 8. Out of scope

* Return tracks, their cache sizing and UI, and sidechain inputs: proposal 55.
* The plugin player and export (DESIGN §10): track P, after Q8.
* Threshold-start recording (DESIGN §7): new recorder work, no Q gate.
* Resonant band-pass and high-pass filters, and a strict round-robin cycle
  (GATES "Later").
* Libraries — base program plus overlay (DESIGN §8): core gate C8; qbx's side
  (a read-only base on disk, greyed orphan edits in the editor) has no Q gate
  and gets one when the author schedules it.
* Programs wider than stereo, or a sampler on a project wider than 2 channels
  (Q-7).
* Plugin delay compensation (37 P9).

---

## 9. Open questions (for the author)

* **Q-1 — Recording and playing one instance. ANSWERED by the author,
  2026-11-01: option (a)**, the instance carries its own record input
  (`audio:<device>:<mask>` on the program), and an armed sampler track with a
  MIDI input records audio into the instrument from it. The question as asked: A track has one input, audio
  or MIDI (`docs/ACTIONS.md:360`). "Speak into the mic, play it on the
  keyboard" needs both on one instrument. Options: (a) the instance carries its
  own record input (an `audio:<device>:<mask>` spelling on the program), and an
  armed sampler track with a MIDI input records audio into the instrument from
  it; (b) a second, audio-input track whose recording is aimed at the sampler
  on another track (`place-sampler-recording` taking a target); (c) a track
  input that may be both. Q6 can be built with the recording on the
  instrument's own audio-input track either way.
* **Q-2 — What is automatable. ANSWERED by the author, 2026-11-01:
  program-level quantities only** (level, tune, bus drives, LFO …) as host
  parameters with fixed, append-only ids; zones are edited with verbs, not
  automated. The question as asked: qbx automates plugin parameters by id
  (`param:` lanes). GATES C5.4 speaks of "automating its drive above 0
  mid-note", which implies zone parameters are automatable — but zones come and
  go, and parameter ids must be append-only (R4). Are only program-level
  quantities (level, tune, bus drives) parameters, or are zone parameters
  exposed under ids derived from the stable zone id?
* **Q-3 — Release tags on nested pins. ANSWERED by the author, 2026-10-26:**
  no nested pins at all. nassau-digital and nassau-dsp become top-level
  nassau-suite submodules, like nassau-plugin-sdk; the suite passes their paths
  to the components; consumers find them by explicit dir, sibling, pinned
  fetch. `ci/release.sh` then tags them unchanged. Applied in A2, B1 and §4.1.
* **Q-4 — "Stored as a project asset". ANSWERED by the author, 2026-11-01:
  both** — the recording is written as a WAV under the project's recordings
  folder AND registered as a qbx asset, so it can also be placed on a lane.
  The question as asked: DESIGN §6 item 2 stores a recording
  "as a project asset". In qbx an asset is a registered body windowing a
  container (`sproject.h:230-236`). This proposal stores the recording as a
  WAV under the project's recordings folder, referenced as an extern file. Is
  that what was meant, or should a recording also be registered as a qbx asset
  (and so be placeable on a lane)?
* **Q-5 — `slot` or `slotIndex`. ANSWERED by the author, 2026-10-27:
  `slotIndex`** (B6 as written; DESIGN and GATES corrected in
  nassau-digital#15). The question as asked: DESIGN §9.3's example and GATES Q1.1 write
  `slot`; the plugin verbs DESIGN cites as the model write `slotIndex`. B6
  takes `slotIndex`; say if `slot` is wanted (accepted as an alias, or
  instead).
* **Q-6 — The slice's rank. ANSWERED by the author, 2026-10-27: keep the
  midi rank**; the shell installs the instance initializer as a hook (B5
  superseded, *Layering* rewritten). The question as asked: DESIGN §9.2 puts `objects/sampler` "at the rank
  of objects/midi" (no edge to `objects/track`). The slot it configures lives in
  `objects/track`, so B5 gives it that one edge and keeps the property that
  matters (the track never depends on the sampler). The alternative — keep the
  midi rank and have the shell install the instance initializer as a hook — is
  possible and costs indirection. Which?
* **Q-7 — Wider than stereo. ANSWERED by the author, 2026-11-01: accepted
  for now**, as for every stereo instrument; out of scope here. The question
  as asked: DESIGN §9.3 says qbx "spreads them on a wider
  one". It does not: 0→2 on more than 2 channels is Unsupported
  (`plugins/CONTRACT.md:262`), for every stereo instrument. Accept that for the
  sampler (out of scope here), or ask for a stereo-to-N spread as qbx work of
  its own?

---

## 10. REVIEW.md, re-verified

Every file:line of nassau-digital `docs/REVIEW.md` this proposal relies on was
checked against this tree on 2026-10-26. They hold, with lines at most a
line or two off. These do not:

| REVIEW.md says | What is true |
|---|---|
| Finding 8c: a 2-output sampler takes "`GenFold` on a mono project, `WideGen` on 6" | `WideGen` is 0→M with **M > C**. Two outputs on six channels is **1 < M < C: Unsupported**, transparent and silent (`plugins/CONTRACT.md:259-262`), gated by `instrument_stereo_render`'s refused row (`:271`). DESIGN §9.3 inherited the error ("spreads them on a wider one") — Q-7 |
| Finding 3: "qbx has no precedent for an external source dependency"; `tw303a/CMakeLists.txt` has no fetch of a sibling repo, "only vcpkg packages" | No sibling-repo dependency, true — but qbx already builds two git submodules from source, `third_party/clap` and `third_party/vst3_pluginterfaces` (`.gitmodules`; `tw303a/CMakeLists.txt:239-251`, `:316`), with CI checking them out recursively (`ci.yml:37-43`) and `_env.sh` fetching them (`:426-449`). §4.1 first followed that precedent; after the author's Q-3 answer it follows the nassau-plugin-sdk one instead (a locator, the suite holds the only pin), and keeps "required, not optional" |
| Finding 4 / G5: the fix is "a `PassVerbs` implementation for the sampler (`place-sampler-recording`)" | `PassVerbs` is the seam a LANE planner emits through, reading a lane snapshot (`recplan/CONTRACT.md:30-60`). A take landing in an instrument has no lane to plan against; `place-sampler-recording` is a sibling verb of `place-recording` submitted by the recorder, not a `PassVerbs` (§4.10) |
| Finding 2: `twSamplerHost` swapped "under the processor mutex", reachable by `dynamic_cast` from the app | True that the app may include `tw/plugins` from `objects/track` (`check_layering.py:383`), but the app has **no way to take that mutex**: `mutex_` is private and `plugin()`/`plugins()` hand out bare pointers (`twpluginslotproc.h:262-263`, `:392`). A new locked entry point is needed (§4.3), and so is an instance initializer, because a rebuild re-instantiates every instance and would drop a pushed snapshot (`twpluginslotproc.cc:392-394`) |
| Finding 9: hand selection "needs a UI tap on the device side" — implying new device work | The tap exists: `MidiInFanout` hands out up to 8 private sinks per port (`midi_in_fanout.h:39-66`). The editor acquires one (§4.11) |
| Not a REVIEW claim but next to it: the ABI's aux buses "reported now so that P9 needs no ABI change" (`plugins` inv. 34) | Reported, yes; but the generator path passes exactly one bus array (`twpluginslotproc.cc:968`), so a plugin reporting more must not write them until the host changes (B7, invariant 10) |

Checked and holding (lines as REVIEW gave them, or within two): `twplugin.h:44-70`
and `:38-39`; `twpluginregistry.cc:707-708`; `twnativeinstrument.cc:116`,
`:222-238`; `spluginslot.h:283`; `spluginslot.cpp:308-312`, `:387-388`;
`check_layering.py:48`, `:142`, `:383`; `plugins/CONTRACT.md:611-619`,
`:921-969`, `:978-979`, `:217-241`, `:255-262`; `shell/CONTRACT.md:812-813`;
`playback/CONTRACT.md:90-109`, `:183-191`, `:192-202`, `:245-253`, `:342-347`; `slivemonitor.cpp:705-706`;
`twliveeventsource.h:72-78`; `recplan/CONTRACT.md:1-7`, `:49-59`;
`wave/CONTRACT.md:105-113`; `sources/CONTRACT.md:18-19`, `:23-31`, `:81-84`,
`:159-163`, `:169-173`, `:231-232`; `twpluginslotproc.cc:954-962`;
`twpagedvocoder.h:110-112`; `sproject.h:280-285`; `FREEZE_PROTOCOL.md:36-39`,
`:116-119`, `:129-140`. The threshold-start absence holds too: neither
`saudiorecorder.cpp` nor `tw303a/record` has a recording threshold.

---

## 11. Corrections from the build

Recorded as the build disproved or refined the text above; the reasoning above
is kept and points here.

### Q0 (QBX-250, 2026-10-27)

1. **"A sibling beside the qbx checkout" needs to be told where the checkout
   is** (§4.1). The canonical locator, like nassau-dsp's, looks for the
   sibling relative to `CMAKE_CURRENT_SOURCE_DIR`, and qbx includes it from
   `smaragd/tw303a/`, two levels below its root, where `../nassau-digital`
   would point INTO qbx. nassau-digital's `cmake/bootstrap/NassauDigital.cmake`
   therefore takes an optional `NASSAU_DIGITAL_SIBLING_ROOT`, which
   `tw303a/CMakeLists.txt` sets to the checkout's root. Consequence worth
   knowing: a qbx worktree under `.claude/worktrees/<slug>` has no sibling at
   that depth and takes the FETCH route; the configure log says so. The
   locator also keeps only an EXPLICIT directory in the cache, unlike
   NassauDSP.cmake, which caches a sibling path and then reports it as
   "explicit" on every later configure.
2. **The round-robin cell is not handed to the core** (§4.2 "Round-robin",
   B10). The core as merged (C3) has no cell size; it has
   `nd::Instrument::startPage()`, "called before the first block of a page".
   `tw/sampler` computes the cell from `twProcessContext::position` exactly as
   B10 says and calls `startPage()` whenever it changes (and after
   `reset()`/`prepare()`), splitting a chunk that straddles a boundary into two
   core blocks. B10's property (a page's choices depend on that page alone)
   holds; invariant 11's "given to the core at prepare" does not describe the
   mechanism. No core change was needed.
3. **`twSamplerHost` has a fifth method, `stats()`, and `collectRetired()`
   returns how many it freed** (§4.2). Q0.2 asks that the installs be "read
   without a lock in `process()`", and a stored value is not evidence of a read:
   `stats()` reports what `process()` itself last ran on (program serial,
   routed mask, installed sample count, active voices), written by `process()`
   as relaxed atomics. It is the one method callable outside
   `applyToInstances`. `twSamplerProgram` and `twSamplerSample` are opaque
   wrappers (a pimpl over `nd::Program` / `nd::Sample`): with the core linked
   PRIVATE (invariant 1), a public header may not name a core type.
4. **The retire list is the core's** (§4.2 "Buffers are never released on the
   audio path"). `nd::Instrument::setSample()` parks a replaced buffer and
   `collectGarbage()` frees it once its use count is 1; `collectRetired()`
   forwards to it. Invariant 5 holds as stated.
5. **No program-level parameters in Q0** (§4.2 "The state blob"). Which
   program quantities become host parameters is Q-2, open; the instrument
   declares none and its blob is the 8-byte header. A saved project with a
   10 MiB sample installed grows by 0 bytes (measured, `sampler_slot_test`).
6. **The render is the sample only to the interpolator's ripple.** The core
   reads every layer through a Kaiser-windowed sinc (cutoff 0.45 fs) even at
   the root pitch, so "the render after the note is the sample" holds for
   band-limited material within the passband ripple (measured 7.8e-4 on a
   0.5-amplitude 440 Hz sine; 1.2e-3 peak on `test_stereo.wav`), and not at all
   near Nyquist. Q0.3 asks for the sample's RMS, which holds (0.49997 / 0.24998
   against the file's 0.49999 / 0.24999); the gates compare renders with
   renders, never "sample for sample".
7. **Q0 needs a test verb §4.5 does not list.** Q0.3 asks a `.qxa` case for
   "the sample's RMS" before the model and its verbs exist (Q1). The test verb
   `sampler-test-install` (testkit) installs a program and one WAV through
   exactly the two §4.3 seams — the slot's instance initializer and
   `applyToInstances` — and writes nothing into the project. It is not undoable
   and not a user verb; Q1's model replaces it as the owner of the program.
8. **`-ffp-contract=off` on the core and on `tw_sampler`** (§4.1 "Compile
   flags"). R7 forbids two flags; byte-identical goldens across x86-64 and
   aarch64 also need FMA contraction off (clang contracts by default), the
   reason `tw_sources` already carries it. The R7 configure assert covers
   `-ffast-math`, `-Ofast`, `/fp:fast`, `-march=native` and `-mcpu=native`.
9. **`tailFrames()`** was not specified: it is the longest amp release in the
   program, extended to the longest installed sample when a zone is one-shot,
   so the pre-roll (capped at 4 s by the processor) reaches back far enough.
10. **`check_layering.py` grants `sources` now; CMake links it in Q4.** §4.2's
    DEPS row is taken as written; the link waits for the first include.
11. **`SPluginSlot::applyToInstances()`** wraps §4.3's two steps
    (`applyToInstances(install)` → `notifyPluginEdited()`) into one call,
    without the second epoch bump `notifyPluginEdited()` would add.


### Q1 (QBX-253, 2026-10-27)

Built: `objects/sampler` (`SSamplerProgram`, the verbs), `tw/sampler`'s
`twSamplerProgramModel`, two shell hooks (`ssamplerwiring.cpp`) and one generic
seam in `SPluginSlot` (model children + a child-adopted hook). The seams as
built, and where the text above did not survive the build:

1. **The seam the Q-6 answer asked for, as built** (§4.4 *Layering*).
   `objects/sampler` has `APP_DEPS = {actions, model, persistence}` (the rank
   of `objects/midi`) and `APP_ENG = base + {plugins, sampler}` — none of the
   asset-bounce, stretch and sidecar edges, which arrive with Q2–Q4. Two
   hooks, both installed by `ssamplerwiring::install()` beside
   `registerNassauDigital()`: `ssampler::setResolver` (trackPath + pathRoot +
   slotIndex → the slot's program, created on first edit) and
   `SPluginSlot::setChildAdoptedHook` (every program that arrives under a
   NassauDigital slot, at run time or from a file, is BOUND: initializer, then
   `applyToInstances`). `objects/track` gained `adoptChild()` and the hook,
   and its loader now attaches the `<SLink>` children it always wrote and
   never read back. Recorded in the three CONTRACTs.
2. **The program is NOT created when the slot is inserted** (§4.4 "created
   when a NassauDigital slot is inserted and when one is loaded"). It is
   created by the first edit that SUCCEEDS (a refused `add-zone` creates
   nothing). A slot without one plays nothing — as one with an empty program
   does — and every Q0 file and golden stays byte-identical. Easily changed if
   the author wants the object from the insert on.
3. **The core's element is nested as CHARACTER DATA, not as elements** (§4.4
   *Persistence*: "nested verbatim … qbx serializes the nested subtree back to
   text"). Measured: QDom writes attributes SORTED BY NAME, so a subtree
   handed back to the core through QDom has its unknown attributes reordered,
   and the core's structural equality (Extras in document order) then calls
   it a different program. Stored as `<program><![CDATA[…]]></program>`, the
   core reads exactly what it wrote. The sabotage that implements §4.4's
   design (nested elements, read back with `QDomElement::save`) fails
   `qxa.sampler_q1_newer_program` at the equality after the reload.
4. **`SPluginSlot` became a CONTAINER element kind.** With a model child, a
   slot has an `<SLink>` that can dangle (a lost or unloadable program); as a
   PLAIN kind the loader would then drop the whole slot and its state chunk.
5. **A program the core refuses on load is kept verbatim** (not in §4.4):
   written back unchanged on save, played as the empty program, every edit
   refused and announced.
6. **Inverses are verbs of their own**: `reinsert-zone`, `reinsert-layer`
   (§4.5's "add-zone restoring the id and every layer", as the core's zone
   XML in CDATA, validated: ids free and `< nextId`) and
   `restore-sampler-values` (§4.5's "set-zone per zone, absolute", one element
   carrying every replaced value). qbx's history RE-APPLIES THE FORWARD action
   on redo, so `add-zone` / `add-layer` remember what they made and re-insert
   it with the same ids; a fresh add on redo would mint new ids and break the
   redo of every later verb that names them.
7. **`nextId` is not restored by an undo** (Q1.1 vs Q1.q2). "Undone exactly"
   and "an id is never reused across an undo" cannot both hold for `nextId`;
   DESIGN §2 decides it. The gates assert the undone `add-zone` equal to the
   program before it in everything but `nextId` (`ignoreNextId="1"`) and
   `nextId` itself explicitly.
8. **A test verb §4.5 lists by another name**: `assert-sampler-program`
   checks structural equality against a file or a CAPTURED program
   (`capture` / `equalsCaptured` / `differsFromCaptured`), named fields, the
   selection, and `installed="1"`, which reads a new
   `twSamplerHostStats::installedSerial` (written by `setProgram()`): what the
   instance HOLDS, so an edit reaching the instrument is asserted without a
   render.
9. **Assumptions awaiting the author** (each easily changed):
   (a) **the selection** (`select-zones`) is model state of the
   `SSamplerProgram`, undoable, changes no sound, and is NOT saved; it holds
   zone ids, and a removed zone drops out of it and comes back with its undo.
   (b) **A negative absolute value** is written with a leading `=`
   (`pan="=-0.5"`): DESIGN §2.1 makes every signed value relative and gives no
   absolute spelling. Text parameters (`src`, `name`) are always literal.
   (c) **Zone ids** come from the core's `nextId` (DESIGN §2) — an `id` on
   `add-zone` is refused; `add-zone` takes its layers as `<layer>` children.
   *(Changed by the author 2026-11-01, see below; built in QBX-265.)*
   (d) **How an edit reaches the engine**: every committed edit publishes one
   new snapshot (initializer, then `applyToInstances`, §4.3), synchronously
   on the main thread.
10. **Not done in Q1, recorded**: `remove-plugin` + undo loses the program (the
    inverse rebuilds a NEW slot from descriptor and blob; a model child is in
    neither). `sampler-test-install` (Q0's test verb) bypasses the model; the
    next model edit on the same slot replaces its initializer.


### The author's answers to the Q1 assumptions (2026-11-01)

Q1 item 9 above: (a) the selection as built, (b) `=` for a negative absolute
value, (d) one full snapshot per edit, synchronously, and §11 Q1 item 2 (the
program object made by the first edit) are **accepted as built**. (c) is
**changed**: `add-zone` MAY name an explicit `id`, accepted when it is free and
has never been used (`>= nextId`, which then moves past it), refused otherwise;
without one the core assigns `nextId` as now. Layers stay `<layer>` children of
`add-zone`.

*Built (QBX-265, 2026-11-01):* the check lives in tw/sampler's
`twSamplerProgramModel::addZone()` (no core change: it sets `nextId` to the
id before the core's `addZone()` assigns it, so the layers follow it). A
layer's `id` is still refused. The two refusals write `zone id N is in use`
and `zone id N was handed out before (nextId M); ids are never reused`; an id
that is a LAYER's counts as in use. Gated by `qxa.sampler_q1_zone_id`;
`sampler_q1_verbs`' refused-id line now uses `id="0"`.

### Core bump to nassau-digital 5adc61a (2026-10-27)

`NASSAU_DIGITAL_GIT_REF` moved from 041456a to 5adc61a (nassau-digital main,
nassau-digital#17). The core now carries gates C5 (drive, filter, kit bus, on
nassau-dsp's filters) and C6 (main + 8 aux outputs); C7 and C8 were already in
at 041456a. The canonical locator `cmake/bootstrap/NassauDigital.cmake` did not
change between the two refs, so qbx's verbatim copy stands.

1. **nassau-dsp is found by the core, not by qbx** (§4.1). The core includes
   its own verbatim `cmake/NassauDSP.cmake` with `NASSAU_DSP_GIT_REF` 76e38fb
   and resolves an explicit `NASSAU_DSP_DIR`, then `../nassau-dsp` beside the
   CORE's checkout, then a fetch. A bare qbx clone (and qbx's CI) fetches both
   (`nassau-dsp: fetched 76e38fb…`, `nassau-digital: fetched 5adc61a…`); the
   sibling workspace finds both as siblings. qbx needs no line of its own for
   nassau-dsp. §7's "a stale sibling builds stale core code" now holds for
   nassau-dsp as well.
2. **The zone filter defaults to `off`** (nassau-digital#17, the author's
   decision of 2026-10-27), a true bypass, so a program that names no filter
   renders exactly as before C5. Measured: `sampler_test` D stays at
   7.8e-4 (L 0.000782686, R 0.000391343) and `sampler_q0_note.wav` is still
   byte-identical; no golden or bound moved. An interim build against d464b79,
   where C5's default was a 24 dB low-pass at 20 kHz, moved D to 1.74e-2 and
   broke the Q0 golden; that ref was never pinned.
3. **B7 holds with no change.** `tw/sampler` calls the core's main-only
   `process(…, outL, outR, n)` overload, under which every zone plays on main
   (an aux output with no buffer counts as unrouted). The routed mask is not
   forwarded to the core yet; when Q8 forwards it, note that the core's mask
   is `twSamplerHost::setRoutedOutputs`'s layout (bit k = aux k + 1), which is
   the slot processor's `setRoutedOutBuses` mask (main = bit 0) shifted right
   by one.


### Q2 (QBX-263, 2026-11-01)

Built: per-layer generations in `SSamplerProgram` (jobs, install, poll,
settle), the offline render's settle in the shell
(`ssamplerwiring::settleBeforeRender`, called from `startRender`), three test
verbs, two cases. Gated: Q2.1, Q2.2, Q2.3 and Q2.q3. Not gated: Q2.q1;
not built: Q2.q2. Where the text above did not survive the build:

1. **The settle is in `startRender()`, not in `beginRun()`** (§4.6 "The
   barrier, offline": "`beginRun()` … first calls
   `ssampler::settleBeforeRun( slot )` — only for an offline render").
   `beginRun()` is shared by the render and the three play-start paths and
   returns nothing, while the settle must REFUSE a render; so
   `startRender()` calls `ssamplerwiring::settleBeforeRender( project, &why )`
   right before `beginRun()`, which keeps §4.6's order (settle, then
   `forgetContinuity()` + `invalidateRenderPathRange()`) and its walk (the
   barrier's own `collectInstrumentTracks` under the master). The log line is
   §4.6's, with `<path>` = `splacements::specOf()` ("0", "Drums:0"), and it is
   written only for a track that HAD an outstanding generation — so a case can
   assert it absent from a render that waited for nothing. ONE bound covers
   the whole render, not one per job or per track.
2. **A file layer's job is a `std::thread` per job, and the main thread
   polls** (§4.6 "computed on a worker (never Qt), posted to the main
   thread"). A worker may not post (THREADING rule 1: no queued call from a
   raw thread), so it stores its result, sets an atomic and notifies a
   condition variable; a 10 ms `QTimer` on the program (alive only while a
   job exists) installs finished ones, and the settle waits on the condition
   variable — the only wait that works while the `render` verb sleeps on the
   main thread. Not the revalidator's analysis lane: a render's
   `pauseBackground()` pauses that lane, and a settle queued behind unrelated
   analysis would wait for it.
3. **The installer takes two initializers** (§4.3's "an install is then:
   `applyToInstances(install)`"). `future` (program + every current
   generation) becomes the slot's instance initializer; `now` carries only
   what changed — the program after an edit (Q1's behaviour, unchanged), or
   `setSample` for each changed layer after a generation. All finished jobs
   go in ONE install (one epoch bump).
4. **`collectRetired()` runs inside every generation install**, under the
   processor's mutex (§4.2 "the app calls it from the main thread after every
   install and from a slow timer"). There is no slow timer: a buffer the
   install just replaced is still parked and is freed by the NEXT install,
   so at most one retired buffer per swap waits. A slow timer would have to
   go through `applyToInstances`, which bumps the param epoch — re-rendering
   the track to free memory.
5. **The install's stale is redundant for offline renders, so it is gated
   on the epoch** (Q2.1). The run barrier invalidates every instrument
   track from the render's start, so a render after an install that staled
   nothing still hears the new sample (measured: the sabotage that installs
   through the bare instances, skipping `applyToInstances`, passes every
   render assertion). The stale matters for PLAYBACK, which a case cannot
   pin. `qxa.sampler_q2_generations` therefore asserts it directly: the
   job is held across the edit (whose own program publish stales too), the
   track's content epoch is captured, and it must have advanced once the
   generation is installed (`assert-sampler-layer
   trackEpochAdvancedSince=`). That sabotage fails there.
6. **Q2.1 and Q2.2 are asserted as byte identity with single-generation
   goldens.** A's golden is `sampler_q0_note.wav` itself — the model's file
   layer renders Q0's `sampler-test-install` byte for byte (measured) — and
   B's is `goldens/sampler_q2_b.wav`, the first render of a session that
   never had another generation. Every render of either sample after a swap,
   an undo, a redo or a 2 → 1 → 2 rebuild equals its golden; each render
   spans 17 pages with the one-shot sounding from the first.
7. **Rule 3's pin is built and NOT GATED** (Q2.q1). The poll installs only
   when `SAppContext::isRenderingActive()` is false (a static
   `SAppContext::hasInstance()` was added so a unit test without an
   application is not tripped by it). No scripted case can make a job finish
   DURING a render: the settle leaves nothing outstanding when a render
   starts, and the `render` verb sleeps on the main thread, so no action and
   no poll runs until it ends. Removing the check passes every gate
   (measured). Only a GUI render, whose event loop keeps running, can reach it.
8. **Test verbs, as built** (§4.5). `set-sampler-render-gate` takes
   `layer`, `ready`, `holdMs` (< 0: until released) and `settleBoundMs`
   (the test override of `SMARAGD_SAMPLER_SETTLE_MS`, which Q2.q3 needs).
   `assert-sampler-layer` reads `generation` (installed), `rendering`,
   `pending`, `installs` and `src` — the Q3.2 `renders` counter arrives with
   the bounce — plus `captureTrackEpoch` / `trackEpochAdvancedSince` (item 5).
   **`wait-sampler-generations`** is new: the settle without a render, so a
   case can install a swap before rendering and keep the barrier's line out
   of renders that should not wait.
9. **Assumptions awaiting the author** (each easily changed):
   (a) a layer's `src` is resolved, not re-encoded: `file:<path>` resolves
   as the MIDI-file verbs resolve an input (absolute; next to the action
   script; next to the project file through `SFilePathRef`; the cwd), and
   the stored text is what the verb was given — relocation and "collect
   files" for sampler layers are not Q2;
   (b) a generation number is per layer and never goes back, so an UNDO
   that writes the old `src` back is a NEW generation (DESIGN §6.1 rule 1:
   "grows with each new buffer"), and a job superseded before it finished
   leaves its number unused;
   (c) a decode that FAILS (missing file, > 2 channels) is not outstanding:
   the layer keeps its previous generation, announced, and the render runs —
   the settle refuses only a job that is still running at the bound;
   (d) a `src` that is not `file:` (`test:…`, empty) REMOVES the layer's
   buffer, installed as a change like any generation.
