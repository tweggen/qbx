# Concept: Plugin Hosting — Effect Inserts on Tracks (CLAP first; VST3 / AU / LV2 / in‑house later)

> **Status: M0–M5 EXECUTED 2026‑07‑26 — M6 (VST3) and M7 (macOS bring‑up) remain OPEN.**
>
> Added after execution; the `Status: OPEN` note and the §Implementation status table
> below are the PRE‑EXECUTION snapshot and are kept as written. What is live now:
>
> | Milestone | State | What landed |
> |---|---|---|
> | M0 | done | First git submodules (`smaragd/third_party/{clap,vst3_pluginterfaces}`), `ensure_submodules()` in `_env.sh` called from both build scripts, the `clap_probe` spike. |
> | M1 | done | The CLAP backend (`twclapmodule`, `twclapplugin`), `prepare()` actually called, 4096‑frame chunking, preview freezes bypassing the plugin, `TW_HAVE_CLAP` PRIVATE, the in‑repo `twtestclap.clap` fixture. |
> | M2 | done | Scanner, `<configDir>/plugincache.json` with sticky `failed`/`timeout` records, per‑platform search paths, out‑of‑process `smaragd_pluginprobe`, the Options → Plugins page, no OS dialog during a scan. |
> | M3 | done | One `twPluginSlotProcessor` + N per‑bus `twPluginInsert` taps; the interleave bug and the silent‑input‑1 bug deleted; the channel‑mismatch table; the two‑tap deadlock invariant. |
> | M4 | done | Slots round‑trip (`id=` + `instantiateFromDomElement` + `<state>` on the QTextStream path), `STrack`↔chain via `pluginChainId` and `SProjectLoader::deferResolve`, `createNullPlugin()` placeholder, `reloadPlugin()`. |
> | M5 | done | The parameter editor wired in (it had never been constructed anywhere), greyed Missing/Unsupported rows with reason tooltip + Reload/Remove, and the last three actions — `set-plugin-bypass`, `reorder-plugin`, `set-plugin-param` (coalescing). `remove-plugin`'s inverse now carries the state chunk. |
> | M6 | OPEN | VST3 backend. Should touch only `smaragd/tw303a/plugins/`. |
> | M7 | OPEN | macOS bring‑up: bundle loading, entitlements, probe in the bundle. |
>
> Defect 4 of §Defects found in the built code was only HALF fixed by M3 and is
> fully closed by M5: `SObject::invalidateRenderPath()` is a no‑op for a plugin
> slot (the chain is not an `SLink` child of its track, so the root‑down walk never
> reaches a slot), so bypass and parameter edits still rendered byte‑identical
> audio after M3. The slot now emits `audioInvalidated()` and `STrack` invalidates.
>
> Per‑milestone detail is in `plan/STATE.md`; the milestone plan is
> `plan/todo/08_PLUGIN_HOSTING_EXECUTION.md`; the invariants are in
> `smaragd/tw303a/plugins/CONTRACT.md` and `smaragd/main/pluginui/CONTRACT.md`.


> **Status: OPEN — execution plan in `plan/todo/08_PLUGIN_HOSTING_EXECUTION.md`.**
>
> Phases 1, 2 and most of 4 are built; phase 3 is partial; **no plugin format backend exists at
> all** — `twPluginRegistry::rescan()` hardcodes the built-in `tw.passthrough` bit-crusher, and
> neither CLAP nor VST3 is referenced anywhere in the build system. See §Implementation status
> below for what is built, and §Defects found in the built code for what has to be repaired
> before the remaining phases make sense.
>
> Paths in this document predate proposal 14 (modularization) in places; the current locations
> are `smaragd/tw303a/plugins/include/tw/plugins/…` and `smaragd/tw303a/plugins/src/…` for the
> engine, `smaragd/main/objects/track/` and `smaragd/main/pluginui/` for the app.

## Objective

Give Smaragd real audio processing on tracks by hosting third‑party (and in‑house)
signal processors. Today a track only sums its children and applies its own
gain/mute (`twTrackMix`, see proposal 05 §0); there is no insert processing. This
proposal adds:

1. A **format‑agnostic plugin host** that loads an effect plugin (n inputs → m
   outputs) and exposes it to the existing DSP graph as one `twComponent`.
2. **Per‑track insert chains** — an ordered list of plugin slots that sit between
   a track's bus sum and its output.
3. The model, undo, serialization, and UI to manage those slots.

Send/aux tracks and **instrument (synth) plugins** are named here for context but
are **out of scope** — the latter is blocked on a MIDI/note model that does not
yet exist (see §Non‑goals and §8).

## Non‑goals (for this proposal)

- **Instrument / synth plugins.** An instrument is a 0‑audio‑in / m‑out + event‑in
  producer that becomes a track's *content*. Smaragd has no note clips or event
  streams yet, so this is gated on a separate sequencing/MIDI proposal. The host
  interface below is designed not to *preclude* it (capability flag + a thin
  note‑input extension), but instruments are not built here.
- **Send / aux / FX‑return tracks.** A natural follow‑on once inserts exist; not
  in this proposal.
- **Parameter automation over the timeline.** Live param edits (with undo) are in
  scope; recording/drawing automation lanes is not.
- **Full plugin‑delay compensation (PDC).** We expose `reportedLatency()` and note
  the hook; tree‑wide compensation is deferred.
- **A new scripting surface.** Plugin actions slot into the proposal 03 action
  model; no new scripting work here.

## Why this fits the existing engine

Three properties of the current code make this tractable rather than a rewrite:

1. **The DSP unit abstraction is already the right one.** Every processor is a
   `twComponent` with N inputs / M outputs, block‑pull processing
   (`calcOutputTo(buf, len, portIdx)`), and dynamic wiring
   (`setInput`/`linkOutput`). A plugin host is just another `twComponent`;
   `twMixer` already demonstrates the N‑input pattern.

2. **Channels are parallel mono wires, indexed by port — not interleaved.** A
   stereo track is `nBusses_ == 2` (the `.qxp` files carry `nBusses='2'`); the
   track builds one `twTrackMix` per bus, and the bus index is threaded down as
   the output‑port `idx` when pulling children (`twtrackmix.cc:110`). So an
   **n‑channel plugin maps cleanly onto a `twComponent` with n input ports + m
   output ports**, one port per channel — exactly the de‑interleaved layout VST3
   and CLAP want internally. No interleaving rework is needed in the graph.

3. **There is a precedent to copy verbatim.** The audio‑driver layer (proposal
   02) is a plain `AudioBackend` interface with per‑platform implementations and
   a `createAudioBackend()` factory, *hosted* by the `twSpeaker` component. The
   plugin layer is the same shape one level up: a plain `twPlugin` backend hosted
   by a `twPluginInsert` component. Same idiom, same file layout
   (`tw303a/plugins/src/` ↔ `tw303a/devices/src/`, post proposal 14).

## Design

### The central decision: composition, one host‑facing interface

We define **one** narrow, host‑facing plugin interface and **compose** it into
**one** host component — rather than giving each format its own `twComponent`
subclass.

The reasoning (settled in discussion): the `twComponent` side of the work — port
counts, de‑interleaving the per‑bus pull into `float**`, the
produce‑once/cache‑and‑serve‑per‑port behaviour, chunking to the plugin's max
block size, bypass, latency reporting — is **identical across formats**. Only a
small surface varies: `process()`, parameter enumerate/get/set, state save/load,
editor, event conversion. Per‑format nodes would immediately have to factor that
shared machinery into a base class with the same ~6–8 virtuals — i.e. the same
abstraction wearing an inheritance costume, but with the format wrapper now
entangled in `QObject`/engine plumbing.

Composition wins concretely because:

- It mirrors the working `AudioBackend` / `twSpeaker` split already in the tree.
- The registry/browser/loader must describe plugins **before any node exists**
  (drawing the browser, the scan cache, deserialization). A cheap
  `twPluginDescriptor` separates *metadata about an available plugin* from *a live
  node in the graph*.
- Format wrappers stay plain C++ objects, unit‑testable without standing up the
  graph, the environment, or Qt.

**The one risk** is the lowest‑common‑denominator trap (flattening every format to
its intersection, losing CLAP note expressions, VST3 parameter buses, AU presets,
…). Mitigation, not avoidance: keep the core interface **host‑facing** (what
Smaragd needs), **narrow**, and **capability‑queried**, with thin optional
extension mixins for the few format‑specific features the model actually consumes
later. This is how CLAP itself is structured (small core + extensions) and the
conclusion JUCE reached (one `AudioPluginInstance`, format specifics hidden
inside).

### Layer 1 — `twPlugin`: the host‑facing interface (plain C++, no Qt)

```cpp
// tw303a/plugins/include/tw/plugins/twplugin.h  — deliberately narrow, host-facing.
// (namespace audio; built as shown below and unchanged since.)
struct twPluginIoLayout {
    std::uint16_t audioInputs  = 0;   // channel counts (mono wires)
    std::uint16_t audioOutputs = 0;
};

struct twPluginParamInfo {
    std::uint32_t id;
    std::string   name;
    double        minValue, maxValue, defaultValue;
    bool          isStepped;
};

class twPlugin {
public:
    virtual ~twPlugin() = default;

    virtual const twPluginIoLayout &ioLayout() const = 0;

    // Real-time: called on the audio thread. De-interleaved, one buffer per
    // channel; nframes <= the value passed to prepare().
    virtual void prepare( std::uint32_t sampleRate, std::uint32_t maxBlock ) = 0;
    virtual void process( const float *const *in, float *const *out,
                          std::uint32_t nframes ) = 0;
    virtual void reset() = 0;

    // Parameters (host-drawn UI + automation/undo).
    virtual std::size_t        paramCount() const = 0;
    virtual twPluginParamInfo  paramInfo( std::size_t i ) const = 0;
    virtual double             getParam( std::uint32_t id ) const = 0;
    virtual void               setParam( std::uint32_t id, double v ) = 0; // RT-safe path

    // Opaque state chunk for serialization.
    virtual std::vector<std::uint8_t> saveState() const = 0;
    virtual bool loadState( const std::vector<std::uint8_t> & ) = 0;

    virtual std::uint32_t reportedLatency() const { return 0; }

    // Capabilities — keep the core narrow; query for the rest.
    virtual bool supportsNativeEditor() const { return false; }
    virtual bool acceptsNotes()         const { return false; } // future: instruments
};
```

Format‑specific behaviour (native editor window, note input) lives behind thin
extension interfaces queried via `dynamic_cast`/capability flags, added only when
a phase needs them.

### Layer 2 — descriptor, registry, factory (mirrors `createAudioBackend`)

```cpp
struct twPluginDescriptor {           // cacheable; exists without a live node
    std::string   format;             // "clap" | "vst3" | "au" | "lv2" | "tw"
    std::string   uid;                // format-stable unique id
    std::string   path;               // module file
    std::string   name, vendor;
    twPluginIoLayout io;
    bool          isInstrument = false;
};

class twPluginRegistry {
public:
    void rescan();                                    // off the audio thread
    const std::vector<twPluginDescriptor> &plugins() const;
    std::unique_ptr<twPlugin> instantiate( const twPluginDescriptor & );
};

std::unique_ptr<twPlugin> createPlugin( const twPluginDescriptor & ); // factory
```

Backends live in `tw303a/plugins/src/` (`twclapplugin.cc`, `twvst3plugin.cc`, …) and
are wired into CMake exactly like the audio backends. The registry caches metadata
to `<configDir>/plugincache.json` (the `ssettings` pattern) so launches don't re‑scan.

### Layer 3 — `twPluginInsert`: the host `twComponent`

Wraps one live `twPlugin`. `getNInputs()/getNOutputs()` report the plugin's channel
layout. Because each bus port is pulled independently by the graph, the component
processes **all channels coherently on the first port pulled per block**, then
serves the rest from cache:

```cpp
length_t twPluginInsert::calcOutputTo( sample_t *dst, length_t len, idx_t port )
{
    if( !producedThisBlock_ ) {
        // Pull every input bus into de-interleaved scratch (one mono wire each).
        for( idx_t c = 0; c < nIn_; ++c )
            pullInput( c, inScratch_[c], len );
        // Chunk to the plugin's max block size; bypass = copy in->out.
        if( bypass_ ) copyChannels( inScratch_, outScratch_, nIn_, len );
        else          plugin_->process( inScratch_.data(), outScratch_.data(), len );
        producedThisBlock_ = true;              // reset when the block advances
        // outScratch_[*] now backs each output latch.
    }
    std::memcpy( dst, outScratch_[port], len * sizeof(sample_t) );
    return len;
}
```

Bypass passes input straight through (matching channel counts).

**Channel‑mismatch policy.** Sensible defaults for mono↔stereo only; anything
wider is explicit routing, never guessed:

| Plugin I/O | Track buses | Behaviour |
|------------|-------------|-----------|
| 1→1 | N (e.g. stereo) | **Dual‑mono** — run the plugin independently per bus (L→L, R→R). Preserves the image; channel‑linked internal state is *not* shared (fine for EQ/filter/distortion). |
| 2→2 | 1 (mono) | Feed the mono wire to both plugin inputs; **average** the two outputs back to mono. |
| 2→2 | 2 / 1→1 | 1 | Direct — the normal cases. |
| >2 channels, or asymmetric in≠out | any | **No auto‑mix.** Explicit user routing only; until a routing matrix exists such a plugin loads bypassed/placeholder rather than guessing. |

### Layer 4 — model: `SPluginSlot` as a track child

Following the `SObject`/`SLink` model (proposals 03/05), a slot is a model object
that owns one insert component:

```cpp
class SPluginSlot : public SObject {            // child of a track's insert chain
    twPluginInsert     *insert_;
    twPluginDescriptor  descriptor_;
    bool                bypassed_ = false;
    // getRootComponent() -> *insert_
};
```

The chain itself is either a dedicated child container (`SPluginChain`) or a
reserved, ordered sub‑list on `STrack`. Recommendation: a `SPluginChain` container
so it reuses child ordering, refcounting, and serialization unchanged, and keeps
inserts visually distinct from clip/child‑track children.

### Layer 5 — wiring change in `STrack`

Today (per bus): `twTrackMix → twRewire`. With inserts (per bus, parallel mono
wires preserved so stereo plugins stay coherent):

```
twTrackMix[bus] → slot0[bus] → slot1[bus] → … → twRewire[bus]
```

The chain is rebuilt whenever a slot is added/removed/reordered/bypassed. This is
the **only** place the existing graph topology genuinely changes; everything
upstream (children summing) and downstream (master mixer, speaker) is untouched.

### Layer 6 — undo + serialization (proposal 03 action model)

New registered actions, path‑addressed like the existing track actions:

| Action | Notes |
|--------|-------|
| `SInsertPlugin`   | insert descriptor at (trackPath, slotIndex) |
| `SRemovePlugin`   | inverse re‑inserts with saved state chunk |
| `SReorderPlugin`  | move within a chain |
| `SSetPluginBypass`| toggle |
| `SSetPluginParam` | coalescing by (slot, paramId), like the fader merge |

Serialization stores the **descriptor** (format + uid + path + I/O) plus the
plugin's opaque **state chunk** as base64. Reload re‑instantiates by uid via the
registry and restores the chunk; a missing plugin becomes an inert
"missing‑plugin" placeholder slot that round‑trips on save (so a project opened
without a given plugin installed does not lose it).

```xml
<SPluginChain>
  <SPluginSlot format='clap' uid='com.example.eq' bypassed='false'>
    <state encoding='base64'>AAECAwQ…</state>
  </SPluginSlot>
</SPluginChain>
```

### Layer 7 — UI

An **FX section** on the track control strip (`SSMVMixerControl`): an ordered slot
list with per‑slot bypass toggles and a "+" that opens a **plugin browser** fed by
`twPluginRegistry`. The first pass uses a **host‑drawn generic parameter editor**
(sliders built from `paramInfo`) — no native plugin windows yet. Drag‑to‑reorder
slots maps to `SReorderPlugin`.

## The hard parts (explicit, so they get scheduled not discovered)

- **Native plugin editor windows.** Embedding a plugin's own GUI
  (HWND/NSView/X11) into Qt via `QWindow::fromWinId` + `createWindowContainer` is
  real per‑platform work. Deferred behind the generic editor; its own phase.
- **Real‑time safety.** `process()` runs on the WASAPI render‑callback thread.
  Scan/instantiate/state‑load are heavy and must stay off it; GUI param edits must
  reach the audio thread without locks (a lock‑free param ring, or routing through
  the action queue). The pull‑from‑callback design already enforces this
  discipline.
- **Crash isolation vs. performance.** Third‑party plugins crash and hang,
  especially during scan. The two reasons to sandbox decouple cleanly:
  *playback* hosting is real‑time and performance‑critical, while *scanning* is
  where crashes actually happen and is not real‑time at all. In‑process playback
  is strictly faster and lower‑jitter: `process()` is a direct call and audio
  never leaves the address space. Out‑of‑process playback must cross a process
  boundary per block — even with shared‑memory audio, the per‑block wakeup
  handshake adds latency and, worse, *jitter* (the fat tail glitches audio), and
  hiding it usually costs ~+1 buffer of latency or strict RT‑priority/core‑pinning.
  **Decision: in‑process playback hosting + sandbox the *scanner*** (cheap, off
  the real‑time path, captures most of the stability win), with the registry
  boundary drawn so out‑of‑process playback can be added later if ever wanted.
- **Latency / PDC.** Plugins report latency; tree‑wide compensation is a real
  feature. Expose `reportedLatency()` now; defer compensation.
- **Channel‑count mismatches.** Up/down‑mix policy when a plugin's I/O ≠ the
  track's bus count.

## Phased rollout

1. **Proof of concept.** `twPlugin` + `twPluginRegistry` + the **CLAP** backend +
   `twPluginInsert`, **plus the `twSpeaker` stereo‑to‑device fix** (decision 3).
   Hard‑code‑load a CLAP plugin onto a track in test code and *hear* it process in
   stereo. Proves the audio path and channel coherency. No UI, no persistence.
2. **Track insert chain.** `SPluginSlot` + `SPluginChain` + `STrack` rewiring +
   bypass; the mono↔stereo channel‑mismatch defaults.
3. **Undo + serialization + scanner/cache.** Actions, XML round‑trip,
   missing‑plugin placeholders, **sandboxed (out‑of‑process) scanner** + metadata
   cache.
4. **UI.** FX strip + plugin browser + generic parameter editor.
5. **Native editor windows** (per‑platform embedding).
6. **More formats** (VST3 / AudioUnit / LV2), each a new backend behind the same
   interface.
7. **Sends / aux tracks** (separate proposal, builds on inserts).
8. **Instrument plugins** — gated on a future MIDI/note proposal; reuses the host
   via the `acceptsNotes()` capability + a note‑input extension.

## Implementation status (2026‑07‑26)

| Phase | State | Notes |
|-------|-------|-------|
| 1 — PoC (`twPlugin`, registry, `twPluginInsert`) | **built, no real format** | Only `twPassThrough` (a 2→2 bit‑crusher) exists. `createPlugin()` was never added; the factory is `twPluginRegistry::instantiate()`. |
| 2 — Track insert chain | **built, defective** | `SPluginSlot` + `SPluginChain` + `twPluginChain`; `STrack` wires `twTrackMix[bus] → twPluginChain[bus] → twRewire[bus]`. Channel‑mismatch policy unimplemented — see defects. |
| 3 — Undo + serialization + scanner/cache | **partial** | 2 of 5 actions (`insert-plugin`, `remove-plugin`). No scanner, no cache, no search paths, no sandbox. Slot serialization is broken — see defects. |
| 4 — UI | **mostly built** | `SPluginEffectStrip` (mounted from `strackdetailpanel.cpp:118`), `SPluginBrowserDialog`, `SPluginParamEditor` — the editor is built but never constructed by anything. |
| 5 — Native editor windows | not started | |
| 6 — More formats | not started | No CLAP/VST3/AU/LV2 dependency exists in the build. |
| 7 — Sends / aux | not started | |
| 8 — Instruments | not started | |

Tests today: `ctest -R plugins_test` (registry/instantiate/param‑count/state round‑trip; it does
**not** exercise audio), `tests/cases/plugin_effect_chain_playback.qxa` (no‑crash), and
`smaragd/tests/cases/render_sawtooth_with_effects.qxa`.

## Defects found in the built code

These must be repaired before the remaining phases are meaningful. Detail and file/line
references are in `plan/todo/08_PLUGIN_HOSTING_EXECUTION.md`.

1. **The signal path is broken for anything but a mono passthrough.** `STrack` builds one
   `twPluginChain` per bus with `nBusses=1`, so a 2‑in/2‑out plugin receives bus audio on input 0
   and *silence* on input 1; `twPluginInsert::freezePage` then writes **interleaved** stereo into
   a page the rest of the engine reads as **mono** (`FRAME_CAPACITY = PAGE_SIZE/sizeof(float)`,
   and `twComponent::freezePage_nolock` renders `idx = 0` only). Invisible today only because the
   built‑in test plugin defaults to a 0.0 dry/wet mix.
2. **`twPlugin::prepare()` is never called anywhere in the repo**, and there is no chunking to a
   plugin's max block size — pages are 65536 frames.
3. **A project containing a plugin slot cannot be loaded.** `SPluginSlot::serializeSelfAttributes`
   never calls the base, so no `id=` attribute is written and `SProjectLoader::createObjects`
   aborts the entire load. There is also no `instantiateFromDomElement` / `registerSObjectClass`,
   `serializeStateChunk()` is dead DOM code on a `QTextStream` write path, `STrack` writes no
   reference to its chain (so a loaded chain is orphaned and deleted), and
   `SPluginChain::getRootComponent()` unconditionally throws.
4. **Parameter and bypass edits are inaudible** — they never bump the content epoch, so the
   cached page is served unchanged. They also bypass the action model
   (`main/pluginui/CONTRACT.md` invariant 3): `SReorderPlugin`, `SSetPluginBypass` and
   `SSetPluginParam` do not exist.
5. **`twPluginChain::freezePage` recursively pulls upstream itself** and only ever reads
   `pInputPlugs_[0]`, bypassing the proposal‑19 `planPage` / `freezePageWithInputs` /
   `requestPage` machinery.

## Decisions (settled) and remaining open items

**Settled:**

1. **First format: CLAP.** MIT‑licensed, modern, one cross‑platform codebase,
   clean C ABI — proves the architecture with the least SDK/licensing friction.
   Later backends behind the same interface: **VST3** (ecosystem reach), **AU**
   (mac), **LV2** (Linux), and the **in‑house** format. CLAP's core+extensions
   shape also informs the host interface (narrow core, capability‑queried).
2. **Hosting: in‑process playback + sandboxed scanner** (see §hard parts). Audio
   path stays in‑process for performance/jitter; the crash‑prone, non‑real‑time
   scan is isolated. Registry boundary leaves out‑of‑process playback open later.
3. **Stereo path: fold the `twSpeaker` fix into phase 1.** Today the render
   callback reads only input port 0 and duplicates it to all device channels, so
   bus 1 (right) is computed and discarded — the device boundary is effectively
   mono. A stereo insert is therefore *inaudible* until the callback pulls port 0→L
   and port 1→R through one resampler per channel and interleaves to the device.
   Contained change, but a hard prerequisite to hearing stereo, so it lands in
   phase 1.
4. **Chain container: `SPluginChain`.** A dedicated child container — reuses child
   ordering, refcounting, and serialization unchanged, and keeps inserts visually
   distinct from clip/child‑track children.
5. **Channel‑mismatch policy:** mono↔stereo defaults only (dual‑mono for 1→1 on a
   stereo track; average‑downmix for 2→2 on a mono track); anything wider is
   explicit routing, never auto‑mixed. Full table in §Design / Layer 3.

*Added 2026‑07‑26, when the execution plan was written:*

6. **Stereo‑coherent processing.** One plugin instance per slot processes **all** buses together,
   via a shared `twPluginSlotProcessor` (plain C++, owns the `twPlugin`, the block chunking and a
   per‑page all‑channel cache) plus one `twPluginInsert` **tap** component per bus (1 in / 1 out).
   This supersedes the built per‑bus‑instance wiring, which cannot host a stereo‑linked plugin.
   It keeps the engine's "one mono page per component" invariant intact — parallel mono wires stay
   parallel component instances — and it is where the channel‑mismatch table of §Layer 3 lives.
   Hard invariant: a tap's `pullUpstreamPage()` must **not** take the tap's own component mutex
   (snapshot the producer under a brief lock, release, then `requestPage()`), or bus 0 gathering
   bus 1 deadlocks against bus 1's own freeze.
7. **Generic parameter editor only in the first delivery.** Reuse `SPluginParamEditor`; native
   `IPlugView` / `clap_plugin_gui` embedding stays phase 5, unchanged.
8. **VST3 SDK sourcing: `vst3_pluginterfaces` as a git submodule** under `smaragd/third_party/`,
   compiling only its handful of `.cpp` files, with our own module loader and host classes
   (`IHostApplication`, `IComponentHandler`, `IPlugInterfaceSupport`, a memory `IBStream`).
   Deliberately **not** `add_subdirectory` on the full SDK: its CMake assumes MSVC/Xcode and calls
   `enable_language(OBJCXX)`, which would migrate `.mm` out of `CMAKE_CXX_SOURCE_FILE_EXTENSIONS`
   and change how `devices/src/coreaudio_input.mm` compiles project‑wide. `smaragd/third_party/`
   sits outside the trees walked by `tools/check_layering.py` and `tools/check_logging.py`. The
   app is already GPL, so the SDK's GPLv3 arm is usable. CLAP is vendored the same way
   (`free-audio/clap`, MIT, header‑only) — this establishes the repo's first submodule convention,
   with an `ensure_submodules()` hook called from **both** `build.sh` and `rebuild.sh` (`build.sh`
   skips `ensure_render_deps`, so a hook placed only there would never run on the common path).

**Previously open, now closed:**

- **Scanner sandbox transport** → a dedicated `smaragd_pluginprobe` executable
  (`plugins/tools/plugin_probe.cc`, links `tw_plugins`) that loads one module, writes descriptor
  JSON to stdout and exits. The registry drives it via `QProcess` with a timeout; a crash or hang
  becomes a `failed`/`timeout` record in the scan cache rather than a dead app, and such records
  are remembered and skipped on later launches unless a forced rescan clears them. The **app**
  supplies the probe path, so the registry stays headlessly testable; on macOS the probe is copied
  into `Contents/MacOS/`.
- **Module discovery paths** → `twPluginSearchPaths::defaults(format)`:
  Windows `%CommonProgramFiles%\CLAP` and `%LOCALAPPDATA%\Programs\Common\CLAP` (plus the `VST3`
  siblings), macOS `/Library/Audio/Plug-Ins/CLAP` and `~/Library/Audio/Plug-Ins/CLAP` (plus
  `…/VST3`), and the `CLAP_PATH` / `VST3_PATH` environment variables. Overridden and extended by a
  user‑editable `plugins/searchPaths` `QStringList` in `SSettings`, surfaced as a new "Plugins"
  page in `SOptionsDialog` with Add/Remove and a live "Rescan now" button. The scan cache is
  `<configDir>/plugincache.json`, keyed per module on `path + size + mtime + scannerVersion`.
  (The QAF sidecar store is deliberately *not* used: its key is a content hash of audio PCM and
  its LRU size cap would silently evict the table.)

## Acceptance criteria

1. A plugin loaded onto a track audibly processes that track's summed output, in
   stereo, with bypass working.
2. Insert/remove/reorder/bypass and parameter edits are all undoable.
3. A project with inserts saves and reloads bit‑faithfully (state chunk
   preserved); a project referencing an uninstalled plugin round‑trips via a
   placeholder without data loss.
4. Adding a second plugin format requires only a new backend in
   `tw303a/plugins/src/` — no changes to `twPluginInsert`, the model, actions, or
   UI.
5. No audio‑thread allocation or locking on the `process()` path; scan/instantiate
   never run on the audio thread.
