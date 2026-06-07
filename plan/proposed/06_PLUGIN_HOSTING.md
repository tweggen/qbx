# Concept: Plugin Hosting — Effect Inserts on Tracks (CLAP first; VST3 / AU / LV2 / in‑house later)

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
   (`tw303a/src/plugins/` ↔ `tw303a/src/audio/`).

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
// tw303a/include/plugins/twplugin.h  — deliberately narrow, host-facing.
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

Backends live in `tw303a/src/plugins/` (`clap_plugin.cc`, `vst3_plugin.cc`, …) and
are wired into CMake exactly like the audio backends. The registry caches metadata
to an INI/JSON file (the `ssettings` pattern) so launches don't re‑scan.

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

**Still open:**

- **Scanner sandbox transport.** Process spawn + pipe/shared‑mem protocol for the
  out‑of‑process scan (phase 3 detail).
- **CLAP module discovery paths.** Standard CLAP search paths per platform + a
  user‑configurable extra‑paths setting (`ssettings`).

## Acceptance criteria

1. A plugin loaded onto a track audibly processes that track's summed output, in
   stereo, with bypass working.
2. Insert/remove/reorder/bypass and parameter edits are all undoable.
3. A project with inserts saves and reloads bit‑faithfully (state chunk
   preserved); a project referencing an uninstalled plugin round‑trips via a
   placeholder without data loss.
4. Adding a second plugin format requires only a new backend in
   `tw303a/src/plugins/` — no changes to `twPluginInsert`, the model, actions, or
   UI.
5. No audio‑thread allocation or locking on the `process()` path; scan/instantiate
   never run on the audio thread.
