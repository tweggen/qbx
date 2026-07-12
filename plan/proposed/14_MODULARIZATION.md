# 14 — Modular Decomposition for Independent Subsystem Development

Status: PROPOSED
Depends on: nothing (restructuring; no behavior change)
Enables: parallel development by humans and by less capable AIs, per-module
review, per-module testing, eventual repo split if ever desired.

## 1. Motivation

The codebase (~27k LOC of implementation) is two monolithic build targets
(`tw303a` static lib, `smaragd` exe) with soft internal boundaries. Today,
safely changing `twTrackMix` requires understanding SCut's reader model,
the freeze protocol, and the action system — nothing *stops* an edit in one
place from silently breaking a contract elsewhere (the 2026-07-12 slip-offset
bug was exactly such a cross-boundary contract loss).

Goal: split the project into **modules in the order of tens**, each of which

- has a small, written contract (public headers + CONTRACT.md),
- can be built and tested alone,
- can be assigned to one human or one AI session without loading the whole
  system into their head/context,
- has its dependency direction *enforced by the build*, not by convention.

## 2. Design principles

1. **Dependency direction is law.** Modules form a DAG; lower layers never
   include upper layers. Enforced via per-module CMake targets and include
   paths (a module can only `#include` what it links).
2. **Contract-first.** Each module's public surface is its `include/` tree
   plus a CONTRACT.md stating invariants, threading rules, and forbidden
   dependencies. Everything under `src/` is private.
3. **The component graph is the central seam** (as suggested): everything
   above it talks to `twComponent`-shaped things; everything below it
   implements them. The five sub-contracts of twComponent (§5.1) are the
   most important documentation in the repo.
4. **Views live with their domain object.** Each document object type (cut,
   wave, track, …) is a *vertical slice*: model class + its inline renderer
   + its type-specific actions in one module, implementing framework
   interfaces (`SObject`, `SObjectRenderer`, `SAction`).
5. **No behavior change during restructuring.** Classes keep their names;
   files move; contracts get written; the qxa suite must pass after every
   phase.
6. **AI-compatible by construction.** A module is sized so its CONTRACT.md +
   public headers + tests fit comfortably in one AI context. The definition
   of done for any module task is mechanical: module builds, its unit tests
   pass, the top-level qxa suite passes.

## 3. Current layering violations (fix first — Phase 0)

These made independent engine development impossible.
**Status: Phase 0 executed 2026-07-12 — the engine no longer includes any
`main/` header** (verified by grep + full qxa suite).

| Violation | Fix | Status |
|---|---|---|
| `render_session.cc` includes `sapplication.h` (calls `SApplication::app().setGlobalLocatorPosRealtime`) | `RenderSession::onPosition` callback; app wires it to the locator in `SApplication::startRender`. | ✅ DONE |
| `recording_session.cc` includes `sapplication.h` | `RecordingParams::startLocatorFrames` + `RecordingSession::onPosition` callback; wired in `SApplication::startRecording`. | ✅ DONE |
| `twspeaker.cc` includes `sapplication.h`, `sproject.h`, `sobject.h` | `audio::PlaybackContext` interface (`audio/playback_context.h`), implemented by `SApplication`, injected via `setPlaybackContext()`. | ✅ DONE |
| `capture_revalidator.cc` includes `sobject.h`, `scut.h` (Open Question 2) | `IRevalidatable` interface (`revalidatable.h`) implemented by `SObject` via thin delegations that preserve the historical dispatch; aspect enum moved to engine `capture_aspects.h` (SCutCaptureAspect kept as alias). | ✅ DONE |
| `twcomponent.h` is a god-header: core types (`sample_t`, `offset_t`, …) + `twLatch`/`twLatchOutput` + `twComponent`, and includes `<qobject.h>`/`QList` | Split into `twtypes.h` (core types) and `twlatch.h` (latch machinery, `QList`→`std::vector`); `twcomponent.h` keeps forwarding includes and is now Qt-free. Files move to `tw/core/`⁄`tw/graph/` paths in Phase 2. | ✅ DONE (Phase 1, 2026-07-12) |
| `QString`/QtCore usage in `twsamplesource`, `twwavinput`, `tw303aenv` | Confine Qt in the engine to **QtCore-only, in `tw/sources` and `tw/schedule` explicitly** (short term), target `std::string`/`std::filesystem::path` at the file-loading boundary (long term). Realtime code must stay Qt-free (already policy, see plan/STATE.md thread-adoption findings). | Phase 6 |

## 4. Module map

Prefix `tw/` = engine (no Qt Widgets, no app includes, realtime-aware).
Prefix `app/` = Qt application. **23 modules total.**

### 4.1 Engine modules (13)

| # | Module | Contents (current files) | Public interface | Depends on |
|---|--------|--------------------------|------------------|------------|
| E1 | **tw/core** | type defs from twcomponent.h (`sample_t`, `offset_t`, `length_t`, `idx_t`, `preview_t`), `twformat`, `twfraction`, `twconvert`, `exc` | `types.h`, `format.h`, `fraction.h`, `convert.h` | — |
| E2 | **tw/pages** | `tw_output_page`, `page_interface`, `io_vector`, `capture_page_pool` | `output_page.h`, `io_vector.h`, `page_pool.h` | core |
| E3 | **tw/graph** | `twcomponent` (base + contract), `twlatch`, `twstreaminglatch`, `tw_freeze_context`, `twview`, `twnegotiator` | `component.h` (5 sub-contracts, §5.1), `latch.h`, `view.h` (incl. `MapPosFn`) | core, pages |
| E4 | **tw/sources** | `twrandomsource`, `twsamplesource`, `twsamplereader`, `twresampledsource`, `twresampler`, `twcapturingsource`, `twloopreader`, `twgrainsource`, `twgrainparams`, `twwavinput`, `twwav` | `random_source.h` (§5.2), `sample_reader.h`, `grain.h`, `wav_input.h` | core, pages, graph |
| E5 | **tw/dsp** | `twosc`, `twsaw`, `twsimplesaw`, `twmoog`, `twwhitenoise`, `twconstant`, `twpipe`, `twtestseq` | one header per unit; all are plain `twComponent`s | core, graph |
| E6 | **tw/mix** | `twtrackmix`, `twmixer`, `twrewire` | `track_mix.h` (clip-entry contract: key = opaque pointer, clip-relative positions, `MapPosFn`), `mixer.h`, `rewire.h` | core, pages, graph |
| E7 | **tw/plugins** | `plugins/twplugin`, `twplugindescriptor`, `twpluginregistry`, `twpassthrough`, `twpluginchain`, `twplugininsert` | `plugin.h` (ABI), `descriptor.h`, `registry.h`, `chain.h` | core, graph |
| E8 | **tw/devices** | `audio/audio_backend` + `wasapi_/alsa_/coreaudio_/null_backend`, `audio_input` + platform inputs, `audio_readahead`, `generation_promise` | `audio_backend.h` (§5.3), `audio_input.h` | core (platform SDKs private) |
| E9 | **tw/sinks** | `audio/audio_sink`, `file_sink`, `playback_sink`, `audio_file_writer` + `wav_/ogg_/mp3_writer` | `audio_sink.h`, `file_writer.h` (§5.4) | core (libsndfile/vorbis/lame private) |
| E10 | **tw/render** | `render_session` — **the rendering engine** | `render_session.h` (§5.5) | core, pages, graph, sinks |
| E11 | **tw/record** | `recording_session` | `recording_session.h` | core, devices, sinks |
| E12 | **tw/playback** | `twspeaker`, `audio_engine` | `speaker.h`, `playback_context.h` (app-implemented interface, Phase 0) | core, graph, devices, sinks |
| E13 | **tw/schedule** | `tw303aenv`, `capture_revalidator` | `environment.h`, `revalidator.h` | core, pages, graph |
| — | **tw/analysis** (fold into testkit or keep tiny) | `audio_analysis` | `analysis.h` | core |

### 4.2 Application modules (10)

| # | Module | Contents (current files) | Public interface | Depends on |
|---|--------|--------------------------|------------------|------------|
| A1 | **app/model** | `sobject`, `slink`, `ssortedobjlist`, `sproject`, `sprojectprops`, `sexternfile`, `sexternfilelist`, `sobjectrenderer` (interface only) | `sobject.h` (incl. `mapTimelineToComponentPos`), `slink.h`, `sproject.h`, `sobjectrenderer.h` | tw/graph, tw/schedule, QtCore/Xml |
| A2 | **app/objects/cut** | `scut`, `scutrndrinline`, actions: split/unsplit/resize/duplicate | via A1 + A4 interfaces | app/model, app/actions, tw/sources, tw/mix |
| A3 | **app/objects/wave** | `splainwave`, `splainwaverndrinline`, `swaveformdraw`, actions: add/remove-sample | 〃 | app/model, app/actions, tw/sources |
| A4′ | **app/objects/track** | `strack`, `strackrenderer`, `strackrndrinline`, actions: add/remove/move/reparent/restore-track, set-track-volume | 〃 | app/model, app/actions, tw/mix |
| A4″ | **app/objects/mixer** | `sstdmixer`, `spluginchain` (model), asset actions (create/place/remove-asset) | 〃 | app/model, app/actions, tw/mix, tw/plugins |
| A4 | **app/actions** | `saction`, `sactionregistry`, `sactionqueue`, `sactionhistory`, `sactionundocommand`, `sactionscript`, `sactionrunner`, `strackpath`, generic actions (set-property, toggles, selection glue) | `saction.h` (§5.6), `registry.h`, `history.h`, `runner.h` | app/model, QtCore |
| A5 | **app/persistence** | `sprojectloader`, save/load actions | `project_loader.h` + FORMAT.md (wire format doc, see proposal 04) | app/model, QtXml |
| A6 | **app/selection** | `sselectionmanager`, selection actions | `selection_manager.h` | app/model, app/actions |
| A7 | **app/timeline-view** | `sstdmixerview`, `sobjecttreeview`, `szoomscrollbar`, `sgridtoolbar`, `strackcolormodifier`, `strackheaderresizer`, `strackdetailpanel`, `ssmvmixercontrol` | `stdmixer_view.h` — hosts `SObjectRenderer`s, never touches concrete object types | app/model (+renderer interface), app/selection, QtWidgets |
| A8 | **app/plugin-ui** | `spluginbrowserdialog`, `splugineffectstrip`, `spluginparamereditor`, `spluginslot` | dialogs keyed on `twPluginDescriptor` | tw/plugins, QtWidgets |
| A9 | **app/services-ui** | `srenderdialog`, `srenderprogress`, `srecordingprogress`, `soptionsdialog`, `soptions` | thin dialogs over tw/render, tw/record params | tw/render, tw/record, QtWidgets |
| A10 | **app/shell** | `sapplication`, `smainwindow`, `ssettings`, `main.cpp` | composition root; the only module allowed to know everything | all |
| A11 | **app/testkit** | assert-audio-energy/peak actions, screenshot action, headless mode glue, `tests/` fixtures + qxa cases, tw/analysis | test recipes doc | app/actions, tw/analysis |

(Count: 13 engine + 12 app slices/modules ≈ 25 — "order of tens" as requested.
A2/A3/A4′/A4″ are the per-object vertical slices; more object types = more
slices, which is exactly the point: adding an object type touches one new
module plus registrations.)

## 5. The key contracts (interface definitions)

These are the boundaries that make independent work safe. Each gets a
CONTRACT.md; the sketches below are the normative content.

### 5.1 `tw/graph/component.h` — the component contract

`twComponent` stays one base class (no inheritance refactor), but the header
documents it as **five separable sub-contracts**, and each override MUST say
which sub-contract it participates in:

```cpp
// (1) POSITION — where the cursor is. Positions are in the component's OWN
// domain. Whoever calls seekTo must speak that domain; timeline-vs-source
// translation happens in twView via MapPosFn (see view.h).
virtual bool isSeekable() const;
virtual int  seekTo(offset_t pos);        // state-preserving position set
virtual void reset() = 0;                 // discard DSP state, position 0

// (2) PULL (streaming) — advance the internal cursor.
virtual length_t calcOutputTo(IOVector& dest, idx_t out); // realtime-safe:
// no allocation*, no locks held across upstream pulls, no Qt.
// (*current code still allocates; contract states the target.)

// (3) FREEZE (random access) — render a page at an explicit position.
// PROTOCOL (normative, from plan/STATE.md 2026-07-11/12):
//   - page->startPosition is authoritative; never trust the live cursor.
//   - contiguous previousPage  => restoreInternalState(prev), keep state;
//     discontinuity            => reset().
//   - THEN always seekTo(startPos) + seekInputStreams(startPos).
//   - Pages are cached per component, keyed by startPos IN THE COMPONENT'S
//     OWN DOMAIN. Callers pass domain-mapped positions (twView does this).
virtual std::shared_ptr<twOutputPage> freezePage(uint64_t startPos, ...);
virtual std::any captureInternalState() const;
virtual void     restoreInternalState(const std::any&);

// (4) TOPOLOGY — inputs/outputs/latches. UI thread only.
virtual idx_t getNInputs/getNOutputs() const;
virtual void  createOutputLatches() = 0;
void setInput(idx_t, twLatchOutput*);

// (5) LIFECYCLE — teardown protocol. ZOMBIE => output silence, never touch
// children; teardown() deregisters from parent and cascades.
virtual void teardown();
```

Threading contract (one paragraph, but load-bearing): UI thread mutates
topology and window params; audio/render threads only pull/freeze; state
handoff is snapshot-based (see SCut double-buffer reader model); **no Qt
signals from non-Qt threads** (established rule).

### 5.2 `tw/sources/random_source.h` — sample data contract

```cpp
class twRandomSource {           // immutable, resident sample data
    virtual length_t length() const;                 // frames, own rate
    virtual int      sampleRate() const;
    virtual idx_t    channels() const;
    // Stateless, lock-free, zero-fills past end. THE random-access primitive.
    virtual length_t read(offset_t src, sample_t* dst, length_t n, idx_t ch) const;
    // Mint an independent cursor (never share twWavInput's shared cursor).
    twSampleReader* acquireReader(tw303aEnvironment&, offset_t initial);
    // Project-rate view; cached per rate; the ONLY rate-conversion seam.
    virtual twRandomSource* viewAtRate(int rate) const;
};
```

### 5.3 `tw/devices/audio_backend.h` — device contract (exists, keep)

`AudioBackend` (callback-pull) and `AudioInput` are already clean interfaces.
Contract doc adds: thread ownership of the callback, xrun expectations,
`AudioConfig` negotiation rules, device-id stability rules. Platform
implementations are **private** to the module — nothing outside tw/devices
may include `wasapi_*.h` etc.

### 5.4 `tw/sinks/file_writer.h` — encoder contract (exists, keep)

`AudioFileWriter` (open/write/close + error string) with `createAudioFileWriter
(format)` factory. Contract adds: writers are single-thread, non-realtime,
must be tolerant of interleaved float32 input at any project rate.

### 5.5 `tw/render/render_session.h` — the rendering engine

The module the user called out, made app-free (Phase 0):

```cpp
struct RenderParams {
    std::string outputPath;
    AudioFormat format;  int quality;
    double startTimeSec, endTimeSec;          // absolute project time
    std::function<void(uint64_t /*absPos*/)> onPosition;   // was: SApplication
    std::function<void(size_t done, size_t total)> onProgress;
    std::function<void(bool ok, const char* err)> onComplete;
};
class RenderSession {
    bool start(twComponent* root, const RenderParams&, uint32_t sampleRate);
    void requestCancel();  bool isRunning() const;  ...
};
// Contract: drives root->freezePage() sequentially page-by-page from
// startTime (absolute positions!), extracts [start,end), writes via
// AudioFileWriter. Owns its thread. Root component must honor §5.1(3).
```

This module is a perfect first "independent AI" assignment: pure engine
deps, measurable output (WAV bytes), full coverage by the qxa suite.

### 5.6 `app/actions/saction.h` — the command surface

Already interface-shaped; the contract makes it the **only** mutation path:

```cpp
class SAction {
    virtual QString name() const = 0;
    virtual SApplyResult apply(SProject*) = 0;   // returns inverse or reject
    virtual void writeXml(QDomElement&) const = 0;
    virtual bool readXml(const QDomElement&, int version) = 0;
};
```

Rules: every user-visible mutation is an action; actions are XML-round-trip
stable (roundtrip test); rejected apply() fails headless tests (2026-07-12);
positions/durations in XML are FRAMES, clip paths comma-separated. This is
the API less capable AIs script against — it deserves the best reference doc
(`app/actions/ACTIONS.md`: one section per verb, attributes, examples —
generated skeleton from the registry).

### 5.7 `app/model/sobjectrenderer.h` — the view-per-object contract

```cpp
class SObjectRenderer {          // implemented by each object slice
    virtual void draw(QPainter&, const InlineRenderContext&) = 0;
    // context supplies: frame window in the OBJECT'S domain (the view
    // framework does timeline->object mapping), zoom, palette, selection.
};
```

`app/timeline-view` may only know `SObject` + `SObjectRenderer`; it must
never `dynamic_cast` to concrete types (today's code mostly obeys this via
`getInlineRenderer()` — the contract makes it a rule).

### 5.8 Cross-module protocol docs (docs/contracts/)

Some invariants span modules; they get their own short documents, referenced
by the module CONTRACT.mds:

- `POSITION_DOMAINS.md` — timeline vs clip-relative vs source domain;
  `twTrackMix` speaks clip-relative; `twView::MapPosFn` translates;
  `SCut::mapTimelineToComponentPos` implements; page caches are keyed in the
  component's own domain. (Content: this week's STATE.md entry.)
- `FREEZE_PROTOCOL.md` — §5.1(3) in full, with the discontinuity rules.
- `THREADING.md` — thread inventory (UI, audio callback, render, record,
  revalidator pool), what each may touch, the no-Qt-off-main rule, the
  snapshot/double-buffer pattern.
- `CLIP_MODEL.md` — ClipEntry keying (opaque SLink key, never component
  identity), duration-change propagation (sender is the SObject).

## 6. Project structure changes

### 6.1 Directory layout (moves only, no renames of classes)

```
smaragd/
  tw/                          # engine (was tw303a/)
    core/     {include/tw/core/, src/, tests/, CONTRACT.md}
    pages/    {…}
    graph/    {…}
    sources/  {…}
    dsp/      {…}
    mix/      {…}
    plugins/  {…}
    devices/  {…}
    sinks/    {…}
    render/   {…}
    record/   {…}
    playback/ {…}
    schedule/ {…}
  app/
    model/        {include/app/model/, src/, tests/, CONTRACT.md}
    objects/
      cut/        {model+renderer+actions for SCut}
      wave/       {…}
      track/      {…}
      mixer/      {…}
    actions/      {framework + generic actions, ACTIONS.md}
    persistence/  {loader + FORMAT.md}
    selection/
    timeline-view/
    plugin-ui/
    services-ui/
    shell/        {main.cpp, SApplication, SMainWindow, SSettings}
    testkit/
  docs/contracts/ {POSITION_DOMAINS.md, FREEZE_PROTOCOL.md, THREADING.md, CLIP_MODEL.md}
  tests/          {cases/*.qxa, fixtures}   # stays top-level: integration
```

Include style becomes path-qualified: `#include "tw/graph/component.h"`,
which makes illegal includes visible in review and greppable in CI.

### 6.2 Build enforcement

One `add_library(tw_graph STATIC …)` per module (OBJECT libs where size
argues for it), with:

```cmake
target_include_directories(tw_graph PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
                                    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(tw_graph PUBLIC tw_core tw_pages)   # ← the DAG, explicit
```

A module physically cannot include a header it doesn't link — dependency
direction stops being convention. Platform SDKs (WASAPI libs, ALSA,
libsndfile, vorbis) become PRIVATE deps of tw/devices / tw/sinks only.
`smaragd` (app/shell) links the app modules; `tw_render_probe`-style tiny
test executables link only their module's subtree.

Add `tools/check_layering.py` (CI + pre-commit): greps `#include` lines
against the declared DAG; fails on violations. Cheap and language-agnostic.

### 6.3 Tests per module

- Engine modules get plain C++ test executables (no Qt event loop):
  tw/core (fraction/convert — exists, move it), tw/pages (IOVector bounds),
  tw/sources (reader/loop/grain against synthetic buffers), tw/mix
  (ClipEntry windows against a scripted component), tw/render (render a
  constant/saw graph to WAV, assert bytes).
- App modules test via the **qxa harness** (now honest about failures) plus
  the roundtrip test for actions.
- The top-level `tests/cases/*.qxa` suite remains the cross-module
  integration gate; every module task must leave it green.

### 6.4 CONTRACT.md template (per module)

```markdown
# <module> — CONTRACT
Purpose: <2-3 sentences>
Public headers: <list — everything else is private>
Depends on: <modules>          Forbidden: <the tempting wrong deps>
Threading: <which threads call in; what is realtime-safe>
Invariants: <numbered; reference docs/contracts/*.md where shared>
How to test: <exact commands: build target + test exe + relevant qxa cases>
Known debt: <list>
```

## 7. Migration plan (each phase leaves qxa green)

- **Phase 0 — break the upward includes** (small, high value):
  callback-injection for render/record sessions, `PlaybackContext` for
  twspeaker. After this, `tw303a` no longer includes `main/` headers.
- **Phase 1 — split the god-header**: extract `tw/core/types.h` and
  `tw/graph/latch.h` from `twcomponent.h`; de-Qt the latch (QList→vector).
  Old header keeps working via forwarding includes.
  ✅ DONE 2026-07-12 as `twtypes.h` + `twlatch.h` (paths get their module
  directories in Phase 2). `twLatchOutput` also gained the virtual
  destructor it always needed (it is deleted through the base pointer).
- **Phase 2 — move files into module directories** with per-module CMake
  targets and path-qualified includes; add `check_layering.py`. No code
  edits beyond include lines. (Biggest diff, zero logic change; do it in
  one sitting to avoid long-lived divergence.)
  ✅ ENGINE SIDE DONE 2026-07-12: tw303a/ is now 14 module dirs
  (`<mod>/include/tw/<mod>/` + `<mod>/src/`), one `tw_<mod>` STATIC lib per
  module with the DAG declared via target_link_libraries, plus an umbrella
  `tw303a` INTERFACE target publishing `compat/` forwarding headers so the
  app compiles unchanged. `tools/check_layering.py` verifies the DAG and
  the no-app-includes rule. Findings fixed en route: twconvert.h and
  io_vector.h illegally included twcomponent.h (needed only twtypes.h);
  `AudioFrame` and `generation_promise` moved to tw/core (shared by
  playback and sinks); playback needs tw_sources (resampler), not tw_sinks;
  the dead `_TW303A_STANDALONE` demo tw303a.cc was retired from the build;
  vcpkg runtime DLLs (libsndfile & co.) are now deployed post-build —
  previously they only survived by accident until a clean rebuild.
  App side (main/ → app/ modules) remains for a follow-up session.
  ✅ APP SIDE DONE 2026-07-12 (structure + canonical includes):
  main/ is now 13 module directories (model, objects/{cut,wave,track,mixer},
  actions, persistence, selection, timeline, pluginui, servicesui, shell,
  testkit) with `app/<module>/…` include paths; all engine includes were
  rewritten to canonical `tw/<module>/…` paths and tw303a/compat/ was
  RETIRED. Finding: the app is ONE strongly-connected component (the
  SApplication singleton, objects creating their own views/detail widgets,
  the loader knowing all types, strackpath in every placement action), so it
  builds as a single OBJECT library `smaragd_app` — deliberately OBJECT, not
  STATIC: the actions self-register via static initializers, which a STATIC
  library would silently drop at link time. Build-level module enforcement
  app-internally is therefore NOT possible yet; tools/check_layering.py now
  carries the declared app edge set (the measured coupling) plus per-module
  allowed engine deps, and flags any new edge. Shrinking that edge list is
  the Phase 6 burn-down (SAppContext interface, loader registry, renderer
  factory). Side benefit: action_roundtrip_test now links smaragd_app
  instead of recompiling every app source; the engine test binaries link
  only tw_core / tw_pages.
- **Phase 3 — write the contracts**: 4 cross-module protocol docs +
  CONTRACT.md per module (much of the content already exists in
  plan/STATE.md; this is distillation, a good AI task per module).
  ✅ DONE 2026-07-12: docs/contracts/{POSITION_DOMAINS,FREEZE_PROTOCOL,
  THREADING,CLIP_MODEL}.md; CONTRACT.md in all 14 engine and 13 app module
  directories; docs/ACTIONS.md (all 41 verbs with attributes, generated
  from the sources); docs/ARCHITECTURE.md (module map + working
  agreement); CLAUDE.md points at all of it.
- **Phase 4 — per-module tests**: move existing test_*.cpp into module
  tests; add the thin engine test exes (tw/render first).
  ✅ DONE 2026-07-12: existing tests moved into <module>/tests/ (core:
  exact_arithmetic + serialization_roundtrip + the previously-unbuilt
  twfraction test; pages: io_vector; plugins: the previously-unbuilt
  plugin-insert test). NEW module tests: sources_test (reader absolute
  seeks, loop window, zero-fill, grain stretch over a synthetic vector
  source), mix_test (ClipEntry windows, MapPosFn slip translation, the
  clip-end clamp, key-based update/remove against a scripted component),
  render_test (RenderSession end-to-end: absolute marked-range content,
  onPosition, page-boundary continuity, via a hand-rolled WAV parser).
  Each test target links ONLY its module subtree. CTest wired at the top
  level: `ctest` runs the 8 unit tests + all 15 qxa cases (23 green).
- **Phase 5 — vertical object slices**: regroup scut/splainwave/strack/
  sstdmixer with their renderers and actions. Mechanical; the framework
  interfaces already exist.
  ✅ DONE 2026-07-12 (regrouping landed with the Phase 2 app split; the
  remaining substance was making slices SELF-CONTAINED): the loader type
  table is populated by self-registration from each slice, SProject
  extern-file creation goes through a registered factory (wave slice), and
  dependency invalidation uses a virtual SObject::invalidateAspects instead
  of dynamic_cast<SCut*>. model now has ZERO app-internal outgoing edges
  and persistence only {actions, model, shell} — first measurable Phase 6
  burn-down, locked in by tools/check_layering.py.
- **Phase 6 (ongoing) — debt burn-down** per module: Qt out of engine
  file-loading, allocation-free calcOutputTo, etc., each tracked in the
  module's CONTRACT.md "Known debt".
  ▶ MAJOR PROGRESS 2026-07-12 — the app SCC is broken into layers:
  `model < actions < {persistence, selection} < objects/* < UI+shell`.
  Mechanisms: SAppContext (narrow app interface in model; SApplication
  implements it; core modules include NO shell headers anymore),
  sdetaileditors (view-widget factory — SStdMixer no longer constructs
  SStdMixerView), sobjectpath.h (generic path helpers + virtual
  SObject::isPathContainer replacing the STrack cast), file re-homing
  (sloadprojectaction→persistence, SPluginSlot→objects/mixer where the
  model object belongs, STrackColorModifier→objects/track), plus stale
  include cleanup. actions now depends on {model} only and lost its engine
  playback dep (toggle-playback goes through
  SAppContext::setPlaybackRunning). Remaining cyclic groups, both honest:
  the four object slices among themselves (placement actions know the
  track tree and the types they create) and the UI+shell top layer. ✅ COMPILE-TIME ENFORCEMENT DONE 2026-07-12: the app builds as four
  layered OBJECT libraries (app_model < app_core < app_objects < app_ui),
  each publishing only its own include dirs and linking only lower layers
  + its APP_ENG engine union (model: core/graph/pages/schedule/sources;
  core: +render; objects: +mix/plugins; ui: umbrella). A deliberate
  model→actions include was verified to FAIL COMPILATION. The executables
  link all four targets directly (object files do not propagate through
  object libraries).
  ▶ PLACEMENT SERVICE 2026-07-12: app/model/splacements.h
  (rootContainer/laneAt/placementAt over SObject::isPathContainer) replaced
  every validation-only STrack/SStdMixer cast in the action code, and the
  misfiled placement/lifecycle/plugin files were re-homed (add/remove-
  sample→cut, track lifecycle actions→mixer, plugin chain/slot/actions→
  track, remove-clip→cut beside its inverse). Small model virtuals covered
  the genuinely type-specific bits (activeLane, volumeDbSnapshot). The
  object slices are now a DAG — wave < cut < track < mixer — leaving
  UI+shell as the app's only cyclic group. Remaining for later: a selection
  service to slim SAppContext; optionally per-slice build targets now that
  the slice DAG allows them.

## 8. Working agreement for independent development

- **Task scoping rule:** a task names ONE module it may modify (plus its
  CONTRACT.md). Touching another module's src/ escalates the task.
- **AI recipe (goes in each CONTRACT.md footer):** read CONTRACT.md → read
  public headers of dependencies (not their src/) → run the module tests →
  implement → module tests + `tests/cases/*.qxa` green → update CONTRACT.md
  "Known debt" if the contract shifted.
- **Contract changes are their own PRs**: if the fix requires changing a
  public header or an invariant, that is a separate, human-reviewed change
  before dependent work proceeds.
- **Ownership matrix** (append to this doc when adopted): module → owner →
  reviewer, so parallel sessions never collide on src/ directories.

## 9. Open questions

1. Rename `tw303a/` → `tw/` now (big rename, do in Phase 2) or keep the
   directory name and only add subdirectories? (Proposal assumes rename;
   keeping it is fine too, the module boundaries matter more than the root
   name.)
2. ~~`capture_revalidator` sits awkwardly between engine and app~~ —
   RESOLVED (Phase 0, 2026-07-12): stays in the engine (tw/schedule) behind
   the `IRevalidatable` interface (`revalidatable.h`), which `SObject`
   implements by delegation. Aspect bits live in engine `capture_aspects.h`.
3. Do we want per-module version pinning (submodules/subtrees) eventually?
   Out of scope here; the CMake DAG gives 90% of the benefit at 10% of the
   cost.
