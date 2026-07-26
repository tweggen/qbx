# tw/plugins — CONTRACT

Purpose: the plugin ABI (twPlugin: prepare/process/reset, params, state
blobs), descriptors, the registry, the SCANNER (search paths, on-disk cache,
out-of-process probe), the format backends, and the hosting chain components
(twPluginChain, twPluginInsert). twPassThrough is the built-in test plugin (a
bit-crusher); twClapPlugin is the CLAP backend (proposal 08 M1); the scanner is
proposal 08 M2.

Public headers: twplugin.h, twplugindescriptor.h, twpluginsearchpaths.h,
twpluginchain.h, twplugininsert.h.

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
8. The registry is thread-safe and its results are handed out BY VALUE.
   plugins() and scanStats() return copies under a mutex because a scan runs on
   a worker thread and REPLACES the descriptor list wholesale; a const& into
   plugins_ was a data race the moment M2 landed. rescan() itself never holds
   that mutex while probing — it snapshots the configuration, works on locals,
   and swaps the result in at the end — so the UI stays answerable during a scan.
9. The scan cache remembers FAILURES, and only rescan( force ) clears them.
   A module is re-probed only when path + sizeBytes + mtimeMs + scannerVersion
   differ; a status of failed or timeout is a cache HIT that yields no plugin.
   Otherwise one crashing or hanging plugin costs a probe (or a timeout) on
   every launch, forever. kScannerVersion is part of the key, so any change to
   what the scanner derives invalidates every record including the failures.
   The cache is <configDir>/plugincache.json, NOT twSidecarStore: that store
   keys on a content hash of audio PCM and caps itself with an LRU, which would
   silently evict the plugin table.
10. Crash isolation is the probe EXECUTABLE, and the app supplies its path.
   setProbeExecutable() is what turns "load foreign code in our address space"
   into "load it in a child process we can bury". The registry does not go
   looking for it — the app knows where it is (next to the exe on Windows,
   inside Contents/MacOS on macOS), and a registry that cannot find its own
   probe would be untestable headlessly. When no probe is configured, or it
   cannot be STARTED (as opposed to failing on a plugin), the scan falls back
   in-process and logs a warning: still correct for a corrupt file, but with no
   protection against a plugin that crashes while being instantiated.
11. twClapModule::open() must not destroy a failed module under the intern
   mutex. ~twClapModule takes that same non-recursive mutex to un-intern
   itself, so releasing the half-built module inside the critical section
   self-deadlocks. Only the failure path reaches it, which is why it survived
   M1 untouched — the M2 scanner is the first code that deliberately hands the
   loader files which are not plugins.

How to test: `ctest -R plugins_scan_test` — the scanner gate: cache miss/hit,
invalidate-on-mtime, the stickiness of a failed record (and that force clears
it), cache reload in a fresh registry instance, refusal of a cache from another
scannerVersion, findByUid, rescanAsync + waitForScan, and the same verdicts
through the out-of-process probe. Its "real module" is the twtestclap.clap
fixture and its "bad module" is a file of garbage named *.clap, so nothing has
to be installed. A module that HANGS (the Timeout record) has no cheap fixture;
it shares the probe's kill path and is covered by the manual pass.

Also `ctest -R plugins_test` — the built-in plugin's descriptor /
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
- Only CLAP is scanned. `formatForFile()` in twpluginsearchpaths.cc maps
  `*.clap` and nothing else ON PURPOSE: a `.vst3` found before M6 lands would
  be probed, fail, and be cached as a permanent failure that M6 would then have
  to force-clear. `twPluginSearchPaths::defaults("vst3")` already returns the
  right directories, so M6 adds one line to the extension table.
- The scan is all-or-nothing per run: there is no incremental "this directory
  changed" trigger and no filesystem watcher, so picking up a plugin installed
  while the app is running needs the Options page's Rescan (or a restart).
- Probing is serial. One module at a time, each a process launch; a machine
  with hundreds of plugins pays for that once (the cache absorbs it afterwards),
  but the first launch is slower than it has to be.
- The Timeout record is written by a code path no automated test reaches: it
  needs a module that hangs inside clap_entry.init(), which no in-repo fixture
  provides.
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
