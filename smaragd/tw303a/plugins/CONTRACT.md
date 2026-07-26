# tw/plugins — CONTRACT

Purpose: the plugin ABI (twPlugin: prepare/process/reset, params, state
blobs), descriptors, the registry, the format backends, and the hosting
chain components (twPluginChain, twPluginInsert). twPassThrough is the
built-in test plugin (a bit-crusher); twClapPlugin is the CLAP backend
(proposal 08 M1).

Public headers: twplugin.h, twplugindescriptor.h, twpluginchain.h,
twplugininsert.h.

Depends on: tw/core, tw/graph. Forbidden: app headers, devices/sinks.

Invariants:
1. Plugin discovery is SYMBOL-referenced (the registry calls
   createPassThroughPlugin() / createClapPlugin() directly) — NOT static-init
   self-registration, so static-library linking is safe here. Keep it that
   way, or move the engine to whole-archive linking first.
2. process() is realtime: no allocation, no locks, no Qt. Everything a render
   needs is sized in prepare(): twPluginInsert's scratch and pointer arrays,
   twClapPlugin's per-port buffers and its event vector.
3. saveState()/loadState() blobs round-trip through project files — versioned
   and tolerant of unknown trailing data. The CLAP backend wraps the plugin's
   opaque chunk in an 8-byte frame ('TWCP', u16 version, u16 reserved); the
   payload after it is the plugin's, so a plugin that grows its own state
   needs no change here. A blob whose version is newer than we understand is
   REFUSED, never guessed at.
4. Format backends stay behind PRIVATE compilation. The clap include
   directory and TW_HAVE_CLAP are PRIVATE to tw_plugins, and the backend's
   header lives in plugins/src/, not plugins/include/. A public
   tw/plugins/*.h whose declarations changed with TW_HAVE_CLAP would give
   tw_plugins and its consumers different views of the same types (ODR/ABI
   skew). Whole translation units are added conditionally in CMake instead —
   the same shape as the ALSA/WASAPI/CoreAudio source lists.
5. The host declares the block size, and honours it. Pages are
   twOutputPage::FRAME_CAPACITY (65536) frames, which no real plugin accepts
   in one call. twPluginInsert::kChunkFrames (4096) is what prepare() promises
   and what freezePage()/calcOutputTo() actually hand to process(), advancing
   through the same de-interleaved scratch so plugin DSP state carries across
   chunks exactly as it would across callbacks in a live host.
6. Preview freezes do not touch the plugin. freezePreviewPage() renders the
   graph at a REDUCED rate (1 kHz) for a waveform envelope; honouring that in
   twPluginInsert would re-prepare() — for CLAP, re-activate and reallocate —
   on every redraw, from a revalidator worker, possibly while playback renders
   the same instance. A freeze whose sampleRate differs from env.getSRate() is
   therefore treated as a preview and copies input to output. The
   authoritative path always passes env.getSRate()
   (twComponent::freezePageWithInputs), which is what makes that comparison
   exact rather than a heuristic.
7. CLAP parameter edits never call the plugin from the editing thread.
   twClapPlugin::setParam() updates a host-side mirror (what getParam() reads)
   and pushes into a lock-free single-producer ring that process() drains into
   clap_process::in_events as CLAP_EVENT_PARAM_VALUE. When the plugin is not
   active — checked under hostMutex_, which prepare()/deactivate also take —
   setParam() flushes through params->flush() on its own thread instead, which
   CLAP permits only in that state. Ring overflow raises a resync flag and the
   next drain re-sends every parameter from the mirror; parameters are
   last-value-wins, so that is always a correct substitute for a backlog.

How to test: `ctest -R plugins_test` — the built-in plugin's descriptor /
param / state surface, plus the real CLAP path (module load, factory,
activate, process, the parameter-event round trip, the state frame's version
tolerance, and 65536-frame pages arriving at the plugin as 4096-frame blocks).
That last one runs against `tests/twtestclap.c`, an in-repo 2-in/2-out CLAP
module built as `twtestclap.clap`: it returns CLAP_PROCESS_ERROR if it is ever
handed more frames than the host declared, and in "report block size" mode
writes the frame count it saw into its output — so a host chunking regression
fails loudly instead of silently. `tools/clap_probe.cc` (target `clap_probe`,
not a gate) loads a real third-party .clap with the production loader and
prints the factory contents. Also qxa.render_sawtooth_with_effects (chain in
the signal path); the plugin browser lists exactly the registry contents.

Known debt:
- The registry still hardcodes the built-in plugin: there is no scanner, no
  search paths, no cache and no out-of-process probe (proposal 08 M2). CLAP
  descriptors have to be built by hand today; `clapModuleDescriptors(path)`
  in the backend's private header is the seam M2's scanner and probe will use.
- The multi-bus signal path is still broken as recorded in
  `plan/todo/08_PLUGIN_HOSTING_EXECUTION.md`: twPluginChain wires only
  `port < nBusses_`, so a 2-in plugin on a 1-bus chain sees silence on input
  1, and twPluginInsert::freezePage writes INTERLEAVED stereo into a page the
  engine reads as mono. M3 replaces the insert with a per-bus tap over a
  shared twPluginSlotProcessor.
- Parameter and bypass changes do not invalidate pages, so an edit is
  inaudible until something else stales the cache (M3).
- CLAP's [main-thread] annotation on activate() is honoured for the common
  case (twPluginInsert prepares in its constructor, which runs on the UI
  thread) but not for a later project sample-rate change, which re-prepares
  from whichever thread renders next.
- Plugin output events (the plugin reporting a moved parameter, gestures) are
  accepted and dropped; no host extensions are offered (get_extension returns
  nullptr for everything). Insert latency is reported, never compensated.
