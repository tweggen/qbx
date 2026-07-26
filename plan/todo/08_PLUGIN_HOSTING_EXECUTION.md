# Plan 08 (execution): Plugin Hosting — CLAP first, then VST3, Win11 → macOS

Execution plan for `plan/proposed/08_PLUGIN_HOSTING.md`. That document holds the *design*
(the `twPlugin` interface, composition over per-format nodes, `SPluginChain` as the container,
the channel-mismatch table, the settled decisions). This document holds *what is actually built,
what is broken, and the milestone order to close it out*.

> **Execution status (2026-07-26): M0, M1, M2, M3, M4 and M5 are DONE; M6 (VST3) and
> M7 (macOS bring-up) are OPEN.** Every §Confirmed problems item below is closed, with
> two carry-overs recorded where they landed: the MONO SINK (`RenderSession` /
> `AudioEngine` collapse the graph's buses to one page and duplicate it) is a
> tw_render / tw_playback gap, not a plugin-layer one, and is nobody's milestone; and
> the FX strip's drag-to-reorder GESTURE cannot fire (`dragSourceIndex_` is never
> assigned; `startDragFromPlugin()` was declared and never defined) — the
> `reorder-plugin` action behind it exists and is tested. Per-milestone detail is in
> `plan/STATE.md`.

## Context

Proposal 08's phases 1, 2 and most of 4 were built and then stalled. The engine has
`twPlugin` / `twPluginInsert` / `twPluginChain` / `twPluginRegistry`; the app has `SPluginSlot` /
`SPluginChain`, two actions (`insert-plugin`, `remove-plugin`), an FX strip mounted in the track
detail panel, a plugin browser dialog and a generic parameter editor. But **no real plugin format
is implemented** — `twPluginRegistry::rescan()` hardcodes one built-in bit-crusher
(`tw.passthrough`), and no CLAP or VST3 SDK is referenced anywhere in the build system.

Target outcome, from the user's point of view:

1. A set of plugin directories is scanned for plugins; sensible per-platform defaults, the list
   is editable and saved in the configuration. The scan is cached between startups. A rescan can
   be triggered.
2. Plugins are selectable in the track detail view and can be added to a track.
3. Plugins are considered in the signal path as expected.
4. Track plugins are serialized in saved files; plugin configuration is saved in the project file
   per plugin instance.
5. A plugin missing on startup is kept in a disabled state so the project stays valid and the
   user can reload it once the plugin is installed.
6. macOS is supported (tested later).

---

## Confirmed problems (from code analysis)

### No scanner

`twPluginRegistry::rescan()` (`smaragd/tw303a/plugins/src/twpluginregistry.cc`) returns one
hardcoded descriptor; `instantiate()` is an `if (uid == "tw.passthrough")`. There are no search
paths, no cache, no mtime tracking, no mutex, no rescan trigger, and no crash isolation. The
browser dialog snapshots `plugins()` once at construction.

### The signal path is broken for anything but a mono passthrough

| Defect | Where |
|---|---|
| A 2-in/2-out plugin gets bus audio on input 0 and **silence on input 1** | `STrack::setNBusses` builds one `twPluginChain` per bus with `nBusses=1` (`strack.cpp:322-352`); `twPluginChain::rebuildWiring` only wires `port < nBusses_` (`twpluginchain.cc:125-130`) |
| `freezePage` writes **interleaved** stereo into a page the engine reads as **mono** | `twplugininsert.cc:252-264` writes `samples[i*2]`/`[i*2+1]`; `tw_output_page.h` defines `FRAME_CAPACITY = PAGE_SIZE / sizeof(float)` (mono frames) and `twComponent::freezePage_nolock` renders `idx = 0` only |
| `twPlugin::prepare()` is **never called anywhere in the repo** | sample rate and max block are never communicated to a plugin |
| No chunking to the plugin's max block size | pages are 65536 frames; most plugins expect ≤ a few thousand |
| `twPluginChain::freezePage` recursively pulls upstream itself and only ever reads `pInputPlugs_[0]` | `twpluginchain.cc:168-206`; bypasses the proposal-19 `planPage`/`freezePageWithInputs`/`requestPage` machinery |

This is invisible today only because the built-in test plugin defaults to a 0.0 dry/wet mix.

### Slots do not round-trip

| Defect | Consequence |
|---|---|
| `SPluginSlot::serializeSelfAttributes` never calls `SObject::serializeSelfAttributes` | no `id=` attribute is emitted → `SProjectLoader::createObjects` hits `if( id.isNull() ) … return -1` and **the entire project load aborts** |
| No `SPluginSlot::instantiateFromDomElement`, no `registerSObjectClass("SPluginSlot", …)` | even with an `id`, the loader logs "Unable to instantiate object of type SPluginSlot" and aborts |
| `serializeStateChunk( QDomElement&, QDomDocument& )` has **zero callers** | the write path is `QTextStream`-based (`SObject::serialize`), not DOM — so the base64 restore in `readPreChildrenAttributes` is dead code |
| `descriptor_.path`, `name`, `isInstrument` are not serialized | proposal 08 §Layer 6 requires format + uid + path + I/O |
| `STrack` writes no reference to its chain, and its ctor always creates a fresh empty one | a loaded `<SPluginChain>` is orphaned; `~SProjectLoader` drops the handle, refcount → 0, `deleteLater()` |
| `SPluginChain::getChainComponent()` is a TODO stub returning a never-assigned member | `getRootComponent()` unconditionally **throws** `std::runtime_error` |
| Loaded slots never fire `slotInserted` on a track | `STrack::onPluginSlotInserted` → `twPluginChain::addPlugin` wiring never runs for a loaded project |

### Missing actions

`SReorderPluginAction`, `SSetPluginBypassAction` and `SSetPluginParamAction` do not exist. Reorder
goes direct (`splugineffectstrip.cpp:262` calls `pluginChain_->reorderSlot(...)`), and bypass and
parameter edits bypass the action system entirely — violating `main/pluginui/CONTRACT.md`
invariant 3. Parameter and bypass changes also never invalidate pages, so an edit is inaudible
because the cached page is served unchanged.

---

## Decisions taken for this execution

| | |
|---|---|
| **Format order** | **CLAP first, VST3 second** — as settled in proposal 08 §Decisions 1. CLAP is MIT, header-only, one C ABI across platforms, zero MinGW risk, so the whole vertical slice (scan → browse → insert → hear → save → reload) lands before any SDK fight. VST3 then proves proposal 08 AC 4 ("a second format is *only* a new backend"). |
| **Stereo** | **Stereo-coherent**: one plugin instance per slot processing all buses together. |
| **Editor** | **Generic parameter sliders only** (reuse the existing `SPluginParamEditor`). Native `IPlugView` / `clap_plugin_gui` embedding stays a separate follow-on. |
| **VST3 SDK** | Git submodule of `steinbergmedia/vst3_pluginterfaces` under `smaragd/third_party/`, compiling only its handful of `.cpp` files; our own module loader + host classes. The app is already GPL, so the SDK's GPLv3 arm is usable. |
| **Scanner isolation** | Out-of-process probe executable, per proposal 08 §Decisions 2. |

Everything else follows proposal 08 unchanged.

---

## M0 — De-risk + third-party wiring — **DONE** (2026-07-26, commit defaf42)

Establishes the repo's first submodule convention (there is none today: no `.gitmodules`, no
`third_party/`, no `FetchContent`).

- `smaragd/third_party/clap` ← submodule `free-audio/clap` (MIT, headers only).
- `smaragd/third_party/vst3_pluginterfaces` ← submodule (used from M6; added now so the
  convention lands once).
- `_env.sh`: new `ensure_submodules()` running `git submodule update --init --recursive` when
  `smaragd/third_party/clap/include/clap/clap.h` is absent. **Call it from both `build.sh` and
  `rebuild.sh`** — `build.sh` deliberately skips `ensure_render_deps`, so a dependency hook
  placed there would never run on the common path.
- Placement outside `smaragd/tw303a/` and `smaragd/main/` is deliberate: `tools/check_layering.py`
  and `tools/check_logging.py` walk exactly those two trees, and SDK sources would trip
  `check_logging.py` on `printf(`.
- `smaragd/tw303a/plugins/tools/clap_probe.cc` — throwaway spike: load one `.clap`, print factory
  descriptors. Confirms Qt's bundled MinGW g++ can load a real plugin before any engine work.
  `<module>/tools/` is the established home for dev tools (cf. `analysis/tools/warp_ab.cc`) and is
  exempt from `check_logging.py`.

## M1 — CLAP backend (`twPlugin` implementation) — **DONE** (2026-07-26, commit defaf42)

New files in `smaragd/tw303a/plugins/`:

- `src/twclapmodule.h/.cc` — module handle. Windows: `LoadLibraryW`. macOS: `.clap` is a bundle,
  so `dlopen(<bundle>/Contents/MacOS/<basename>)` (CFBundle not required). Resolves the exported
  `clap_entry` symbol, calls `init(path)` / `deinit()`, refcounts so one module can back several
  instances, exposes `get_factory(CLAP_PLUGIN_FACTORY_ID)`.
- `src/twclapplugin.cc` — `class twClapPlugin : public audio::twPlugin`, mapping:
  - `ioLayout()` ← `CLAP_EXT_AUDIO_PORTS` (`count`/`get`, main in/out port channel counts)
  - `prepare()` ← `activate(sampleRate, minFrames, maxFrames)` + `start_processing()`
  - `process()` ← `clap_process` with de-interleaved `clap_audio_buffer::data32`
  - `reset()` ← `clap_plugin::reset()`
  - params ← `CLAP_EXT_PARAMS` (`count`/`get_info`/`get_value`); **`setParam()` must not call the
    plugin directly** — it enqueues a `CLAP_EVENT_PARAM_VALUE` into a lock-free ring drained into
    `clap_process::in_events` (and `params->flush()` when not processing)
  - state ← `CLAP_EXT_STATE` `save`/`load` over `clap_ostream`/`clap_istream` shims over
    `std::vector<uint8_t>`, wrapped in our own 8-byte version header so the blob is
    version-tolerant (`plugins/CONTRACT.md` invariant 3)
  - `reportedLatency()` ← `CLAP_EXT_LATENCY`; `supportsNativeEditor()` ← `CLAP_EXT_GUI` present
  - minimal `clap_host` (name/version/`get_extension` returning nullptr for now,
    `request_restart`/`request_process`/`request_callback` recorded as flags, never calling back
    into the graph from the audio side)
- CMake, following the `TW_HAVE_RUBBERBAND` template verbatim
  (`smaragd/tw303a/CMakeLists.txt:111-141`): discovery → `target_include_directories(tw_plugins
  PRIVATE …/third_party/clap/include)` → `target_compile_definitions(tw_plugins PRIVATE
  TW_HAVE_CLAP=1)` → `message(STATUS/WARNING)`. **`PRIVATE`, and the `#ifdef` never enters a
  public `tw/plugins/*.h`** — otherwise ODR/ABI skew across modules.

`twPluginRegistry::instantiate()` gains a `format == "clap"` branch alongside the existing
`tw.passthrough`. Discovery stays **symbol-referenced**, not static-init self-registration
(`plugins/CONTRACT.md` invariant 1).

Also in M1, because they are prerequisites for hearing anything correct:

- Call `prepare()`. Nothing does today.
- **Chunk to the plugin's max block size.** Pages are 65536 frames; process in `maxBlock`
  chunks (default 4096, clamped by what the plugin accepts).
- **Preview freezes bypass plugin processing.** `freezePreviewPage` passes a *reduced* sample
  rate; honouring it would re-`prepare()` (and reset) the plugin on every waveform redraw.
  Preview only needs envelope shape.

## M2 — Scanner, cache, search paths, rescan (AC 1) — **DONE** (2026-07-26, commits a84bfae + 0295b6d)

**Engine** — `smaragd/tw303a/plugins/`:

- `twPluginRegistry` grows: `setSearchPaths(std::vector<std::string>)`,
  `setCachePath(std::string)`, `setProbeExecutable(std::string)`, `rescan(bool force)`,
  `findByUid(format, uid)`, and a `scanProgress` callback. Default search paths come from a new
  `twPluginSearchPaths::defaults(format)`:
  - Windows: `%CommonProgramFiles%\CLAP`, `%LOCALAPPDATA%\Programs\Common\CLAP`
    (+ the `VST3` siblings from M6), plus `CLAP_PATH` / `VST3_PATH`
  - macOS: `/Library/Audio/Plug-Ins/CLAP`, `~/Library/Audio/Plug-Ins/CLAP` (+ `…/VST3`)
- **Cache**: `<configDir>/plugincache.json` (QJson via `tw_core`'s existing Qt Core link; the
  scan path is not realtime). One record per module file: `path`, `sizeBytes`, `mtimeMs`,
  `scannerVersion`, `status` ∈ `ok|failed|timeout`, and the descriptors found. A module is
  re-probed only when `path+size+mtime+scannerVersion` differ. `failed`/`timeout` records are
  **remembered and skipped** so one bad plugin does not cost a probe on every launch; `force`
  clears them. Note the sidecar store (`twSidecarStore`) is deliberately *not* used: its key is a
  content hash of audio PCM, and its LRU size cap would silently evict the table.
- **Out-of-process probe**: new `add_executable(smaragd_pluginprobe plugins/tools/plugin_probe.cc)`
  linking `tw_plugins`; loads one module, writes descriptor JSON to stdout, exits. The registry
  drives it with `QProcess` + timeout; a crash or timeout becomes a `failed`/`timeout` cache
  record instead of taking the app down. The **app** supplies the probe path (registry stays
  dumb and headlessly testable); on macOS a POST_BUILD step copies it into
  `$<TARGET_BUNDLE_DIR:smaragd>/Contents/MacOS/`.
- Scan runs on a worker thread, never the UI thread.

**App**:

- `SOpt` keys + defaults: `plugins/searchPaths` (QStringList — `SSettings::recentProjects()` is
  the existing QStringList precedent), `plugins/scanOnStartup`. `SOpt::def()` needs its first
  platform-conditional default.
- `SOptionsDialog` (`smaragd/main/servicesui/`): new "Plugins" page —
  `tree_->addTopLevelItem` + `stack_->addWidget(buildPluginsPage())` **in matching order** (the
  mapping is by top-level index), plus `buildPluginsPage()/loadPluginsPage()/applyPluginsPage()`
  and `applyPluginsPage()` added to `apply()`. Contents: directory list + Add/Remove
  (`QFileDialog::getExistingDirectory` + the existing `SSettings::lastDir` convention), a
  "Rescan now" button (live action, like the Log page's live `setConsole`), and a status label
  ("N plugins, M modules skipped").
- `SApplication` startup: push search paths + cache path + probe path into the registry, load the
  cache, and scan changed modules in the background.
- `SPluginBrowserDialog` currently snapshots `plugins()` at construction — repopulate on rescan
  completion, and add a filter box + format column.
- `tools/check_layering.py`: `APP_ENG` must gain `'servicesui': … | {'plugins'}` and
  `'shell': … | {'plugins'}`. This is the hand-maintained mirror of the CMake DAG — **two-place
  edit** (also `smaragd/tw303a/CMakeLists.txt` if module DEPS change).

## M3 — Stereo-coherent signal path (AC 3) — **DONE** (2026-07-26, commit 25f74e8)

The frozen-page model is **one mono page per component** (`twComponent::freezePage_nolock` renders
`idx = 0` only). Parallel mono wires are therefore modelled as parallel *component instances* —
which is why `STrack` builds one `twTrackMix` and one `twPluginChain` per bus. Keeping that
invariant, split the slot into **one shared processor + N per-bus tap components**:

- **`twPluginSlotProcessor`** (new, plain C++, not a `twComponent`): owns the `twPlugin`
  instance(s), the bypass flag, `prepare()` state, block chunking, and a small
  `(startPos, len, epoch) → all-channel output` cache. `pageFor()` renders on the first tap that
  asks for a page and serves the rest from cache.
- **`twPluginInsert`** becomes the per-bus tap: 1 in / 1 out, holds
  `shared_ptr<twPluginSlotProcessor>` + `busIndex_`. Its `freezePage` calls `proc->pageFor(...)`,
  passing a gather callback; the processor pulls each bus's upstream page through sibling taps'
  `pullUpstreamPage()`.
- **Channel-mismatch policy** (proposal 08 §Layer 3 table) lives in the processor: `N→N` direct;
  `1→1` on N buses → N plugin instances (dual-mono, so the processor takes an instantiation
  *factory*, not a single instance); `2→2` on a mono track → feed both inputs, average outputs;
  anything else → slot enters `Unsupported`, loads transparent, logs once.
- **Delete the interleaving bug**: each tap writes its own channel as mono into its page.

Two hard invariants to write down and test, because they are exactly the failure classes already
recorded in this repo's history:

1. **`pullUpstreamPage()` must not take the tap's own component mutex.** Snapshot the producer
   `shared_ptr` under a brief lock, release, then call `producer->requestPage()`. Otherwise tap 0
   holding the processor mutex while gathering bus 1 deadlocks against tap 1's own `freezePage`
   waiting on the processor mutex.
2. Use `requestPage()` (the proposal-19 dedup front door), not raw `freezePage()`, and let taps
   inherit the base `planPage()` so the scheduler binds their single upstream dep.

Also in M3: parameter and bypass changes must **invalidate pages**
(`bumpContentEpoch()` / `invalidatePagesInRange()` on the chain + `invalidateRenderPath()`), or
edits are inaudible because cached pages are served. And `twPluginChain::calcOutputTo` holds
`pluginsMutex_` — safe only because the RT callback never renders (`twRtThreadGuard`); leave a
comment saying so.

## M4 — Serialization + missing-plugin placeholder (AC 4, AC 5) — **DONE** (2026-07-26, commit 5a7cfc5)

- `SPluginSlot::serializeSelfAttributes`: **call `SObject::serializeSelfAttributes(o)` first**
  (this is what emits `id=`; without it the whole project load aborts), then `bypassed`,
  `format`, `uid`, `name`, `vendor`, `path`, `nIn`, `nOut`, `isInstrument`.
- Override `SPluginSlot::serialize(QTextStream&)` to emit the `<state encoding='base64'>` child
  element, pulling a fresh blob from the live plugin at save time. This replaces the dead
  DOM-based `serializeStateChunk()`. Deterministic output — `serialization_roundtrip_test`
  asserts byte-equivalence.
- `SPluginSlot::instantiateFromDomElement` + `registerSObjectClass("SPluginSlot", …)`, modelled
  on `STakeStack` (`smaragd/main/objects/cut/src/stakestack.cpp:282-333`). Rebuilds a descriptor
  from the attributes and resolves it against the registry by `(format, uid)`.
- **`STrack` ↔ chain link.** `STrack` serializes `pluginChainId='<ptr>'`. Adoption must be
  deferred: the loader's multi-pass ordering only guarantees resolution for `<SLink objectId>`
  children, and the chain is referenced by a plain attribute. Add a small general hook —
  `SProjectLoader::deferResolve(std::function<void()>)`, run after `createObjects()` — and have
  `STrack::readPostChildrenAttributes` register a lambda that swaps in the loaded chain (dropping
  the ctor-created empty one), reconnects the `slotInserted/slotRemoved/slotsReordered` signals,
  and rebuilds the DSP chain for every bus.
- **Missing / failed plugins**: `enum class twPluginSlotState { Active, Missing, Unsupported }`.
  Add `createNullPlugin(const twPluginIoLayout&)` to `tw_plugins` — an inert passthrough with the
  descriptor's *declared* I/O, so the graph shape stays valid and the chain is transparent. A
  `Missing` slot keeps `descriptor_` and `savedState_` **verbatim** and re-serializes them
  unchanged. `SPluginChain::getChainComponent()`/`getRootComponent()` must stop throwing.
- After a rescan that finds a previously-missing plugin, the slot can be re-instantiated (state
  chunk re-applied) — surfaced as a per-slot "Reload" affordance in M5.

## M5 — UI + undo (AC 2, remaining proposal-08 actions) — **DONE** (2026-07-26)

- Wire `SPluginParamEditor` (built, currently never constructed) into `SPluginEffectStrip`:
  double-click a slot opens it. The strip is already mounted from
  `smaragd/main/timeline/src/strackdetailpanel.cpp:118`.
- Missing/unsupported slots render greyed with the stored plugin name, a reason tooltip, and
  Reload/Remove.
- The three missing actions, on the `sinsertpluginaction.cpp` template:
  `SSetPluginBypassAction`, `SReorderPluginAction` (the strip currently calls
  `pluginChain_->reorderSlot()` directly at `splugineffectstrip.cpp:262`), and
  `SSetPluginParamAction` coalescing by `(slot, paramId)` like the fader merge.
  Check that `SRemovePluginAction`'s inverse carries the state chunk.

## M6 — VST3 backend (proves proposal 08 AC 4) — **OPEN**

Should touch **only** `smaragd/tw303a/plugins/` — no changes to the processor/tap, model, actions
or UI. If it does, that is the finding.

- `src/twvst3module.h/.cc` — bundle-aware loader. Windows: a `.vst3` may be a plain DLL *or* a
  bundle (`Foo.vst3/Contents/x86_64-win/Foo.vst3`); resolve, `LoadLibraryW`, call
  `InitDll`/`ExitDll` if exported, resolve `GetPluginFactory`. macOS: CFBundle +
  `bundleEntry`/`bundleExit`.
- `src/twvst3plugin.cc` — `IPluginFactory{,2,3}` enumeration (`kVstAudioEffectClass`, instrument
  detected from `subCategories`), `IComponent` + `IAudioProcessor` + `IEditController` connected
  via `IConnectionPoint` both ways, `setBusArrangements` + `activateBus` + `setupProcessing`
  (`kSample32`, our chunk size, `kRealtime`), `setProcessing(true)`, `process(ProcessData)` with
  de-interleaved `AudioBusBuffers`. Params from `IEditController::getParameterInfo`.
  **`setParam` must queue into `ProcessData::inputParameterChanges`** — writing the controller
  alone never reaches the processor. State = `IComponent::getState` + `IEditController::getState`
  over our own `IBStream` memory shim, both in one versioned blob.
- Host classes we own (small, MinGW-safe): `IHostApplication`, `IComponentHandler`,
  `IPlugInterfaceSupport`, the memory `IBStream`.
- Compile only `pluginterfaces/base/{funknown,coreiids,ustring,conststringtable}.cpp` from the
  submodule (exact list confirmed by the M0-style spike). Deliberately avoid `add_subdirectory` on
  the SDK: its CMake assumes MSVC/Xcode and calls `enable_language(OBJCXX)`, which would migrate
  `.mm` out of `CMAKE_CXX_SOURCE_FILE_EXTENSIONS` and change how the existing
  `devices/src/coreaudio_input.mm` is compiled project-wide. Add `-w` for the SDK sources (the
  project sets a global `-Wall`) and keep `AUTOMOC OFF` on that target.
- MinGW ABI note: VST3's interfaces are single-inheritance chains from `FUnknown`, and x64 Windows
  has one calling convention — MSVC-built plugins are loadable from a MinGW host (Ardour does
  exactly this). A spike is still the gate before writing the wrapper.

## M7 — macOS bring-up — **OPEN**

Written cross-platform throughout; this milestone is verification plus the mac-only bits.

- CLAP/VST3 bundle loading verified; default search paths under `/Library/Audio/Plug-Ins/`.
- `smaragd/main/smaragd.entitlements` needs `com.apple.security.cs.disable-library-validation` —
  the POST_BUILD `codesign --force --deep --sign -` step otherwise refuses to load unsigned
  third-party bundles.
- `smaragd_pluginprobe` copied into the app bundle.
- No dependency automation exists on macOS (`ensure_render_deps` returns early for non-Windows),
  so the submodules are the whole story — nothing to `brew install`.

---

## Files touched (representative)

**Engine** `smaragd/tw303a/plugins/`
- new: `include/tw/plugins/twpluginslotproc.h`, `src/twpluginslotproc.cc`,
  `src/twclapmodule.{h,cc}`, `src/twclapplugin.cc`, `src/twnullplugin.cc`,
  `src/twpluginsearchpaths.cc`, `src/twpluginscancache.cc`,
  `tools/plugin_probe.cc`, `tools/clap_probe.cc`
- new (M6): `src/twvst3module.{h,cc}`, `src/twvst3plugin.cc`, `src/twvst3host.{h,cc}`
- changed: `include/tw/plugins/twplugindescriptor.h` (registry API), `src/twpluginregistry.cc`,
  `include/tw/plugins/twplugininsert.h` + `src/twplugininsert.cc` (becomes the per-bus tap),
  `src/twpluginchain.cc` (multi-bus freeze, `requestPage`, epoch forwarding), `CONTRACT.md`

**App** `smaragd/main/`
- `objects/track/src/spluginslot.cpp` + header (serialization, slot state, processor ownership)
- `objects/track/src/spluginchain.cpp` (chain component, no more throw)
- `objects/track/src/strack.cpp` (chain id + deferred adoption; per-bus tap wiring)
- `objects/track/src/s{setpluginbypass,reorderplugin,setpluginparam}action.cpp` (new)
- `persistence/src/sprojectloader.cpp` + header (`deferResolve`)
- `servicesui/src/soptionsdialog.cpp` + header (Plugins page), `servicesui/…/soptions.{h,cpp}`
- `shell/src/{sapplication,ssettings}.cpp` (search paths, startup scan)
- `pluginui/src/{spluginbrowserdialog,splugineffectstrip}.cpp` (refresh, param editor, missing UI)

**Build / tooling / docs**
- `.gitmodules`, `smaragd/third_party/{clap,vst3_pluginterfaces}`
- `_env.sh` (`ensure_submodules`), `build.sh`, `rebuild.sh`
- `smaragd/tw303a/CMakeLists.txt`, `smaragd/main/CMakeLists.txt`, `smaragd/main/smaragd.entitlements`
- `tools/check_layering.py` (`APP_ENG` for `servicesui`, `shell`)
- `plan/proposed/08_PLUGIN_HOSTING.md`, `plan/STATE.md`,
  `smaragd/tw303a/plugins/CONTRACT.md`, `smaragd/main/pluginui/CONTRACT.md`, `CLAUDE.md`

---

## Verification

Gates, in the order they should be run:

1. `python tools/check_layering.py` and `python tools/check_logging.py` — both clean.
2. `ctest -R plugins_test` — extend the existing test (it never exercises `calcOutputTo` today)
   to assert actual audio through a chain, plus a new `plugins_scan_test` for cache
   hit/miss/invalidate-on-mtime and `failed`-record stickiness.
3. `ctest -R serialization_roundtrip_test` — must stay byte-equivalent with slots present.
4. New qxa cases in `tests/cases/`, run **from `tests/cases/`** (fixtures are CWD-relative):
   - `plugin_stereo_chain.qxa` — insert a 2→2 plugin on a stereo track, render, assert L≠R and
     per-channel RMS (catches the interleave bug and the silent-right-input bug).
   - `plugin_missing_placeholder.qxa` — load a project referencing an unknown uid; assert the load
     succeeds, audio passes through transparently, and re-save reproduces the descriptor and state
     chunk unchanged.
   - `plugin_bypass_and_param.qxa` — bypass toggle and a param edit each change the rendered
     output (i.e. page invalidation actually fires).
5. Determinism: byte-level `cmp` of rendered 16-bit-PCM WAVs across two runs and across
   `SMARAGD_REVAL_WORKERS` ∈ {1,4,8,16}; then
   `smaragd/tests/repeat_test.sh <bin> plugin_stereo_chain.qxa 50` swept over the same worker
   counts (the flake gate — this is where the M3 deadlock invariant gets proven).
6. Manual on Win11: `./rebuild.sh`, install a real CLAP plugin (Surge XT / Vital / u-he), open
   Edit → Options → Plugins, confirm the scan finds it and the cache makes the second launch
   instant, add it from the track detail FX strip, hear it in stereo, edit params, save, reopen;
   then rename the plugin folder and reopen to see the disabled placeholder, restore it and rescan
   to see it reload. Repeat with a VST3 after M6, and the whole sequence on macOS in M7.
7. Crash isolation: point a search path at a deliberately corrupt `.clap`/`.vst3` and confirm the
   app survives, the module is recorded `failed`, and it is not re-probed next launch.

## Known deferrals (unchanged from proposal 08)

Native plugin editor windows; plugin-delay compensation (`reportedLatency()` is exposed, not
compensated); instrument/synth plugins (needs the MIDI/note model); send/aux tracks; automation
lanes; out-of-process *playback*; AU and LV2 backends.
