# tw/plugins — CONTRACT

Purpose: the plugin ABI (twPlugin: prepare/process/reset, params, state
blobs), descriptors, the registry, the SCANNER (search paths, on-disk cache,
out-of-process probe), the format backends, and the hosting components
(twPluginChain, twPluginSlotProcessor, twPluginInsert). twPassThrough is the
built-in test plugin (a bit-crusher); twClapPlugin is the CLAP backend
(proposal 08 M1); the scanner is proposal 08 M2; the processor/tap split is
proposal 08 M3; twNullPlugin (createNullPlugin) is the missing-plugin
placeholder of proposal 08 M4.

Shape of a slot (proposal 08 M3). ONE twPluginSlotProcessor per slot (plain
C++, not a twComponent) owns the twPlugin instance(s), the bypass flag, the
prepare() state, the block chunking, the channel-mismatch mapping and a small
(startPos, len, stamp) -> all-bus page cache. Around it sit N twPluginInsert
TAPS, one per track bus, each strictly 1 in / 1 out. The taps are what the
graph sees; channel coherence is what the processor provides. STrack builds one
twPluginChain per bus, and each chain holds that bus's taps in slot order.

Public headers: twplugin.h, twplugindescriptor.h, twpluginsearchpaths.h,
twpluginchain.h, twplugininsert.h, twpluginslotproc.h.

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
   in one call. twPluginSlotProcessor::kChunkFrames (4096, re-exported as
   twPluginInsert::kChunkFrames) is what prepare() promises and what the
   processor's render actually hands to process(), advancing through the same
   de-interleaved scratch so plugin DSP state carries across chunks exactly as
   it would across callbacks in a live host.
6. Preview freezes do not touch the plugin. freezePreviewPage() renders the
   graph at a REDUCED rate (1 kHz) for a waveform envelope; honouring that in
   twPluginInsert would re-prepare() — for CLAP, re-activate and reallocate —
   on every redraw, from a revalidator worker, possibly while playback renders
   the same instance. A freeze whose sampleRate differs from env.getSRate() is
   therefore treated as a preview: the tap forwards its upstream page and never
   reaches the processor. The authoritative path always passes env.getSRate()
   (twComponent::freezePageWithInputs), which is what makes that comparison
   exact rather than a heuristic. The preview page is deliberately NOT entered
   into the component's outputPages_ either -- it is at the wrong rate for any
   other consumer.
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
12. A scan NEVER puts a dialog on screen. Windows answers a truncated or
   wrong-architecture DLL with a MODAL "Bad Image" box (0xc000012f) from inside
   LoadLibrary, and a crash inside clap_entry->init with the Windows Error
   Reporting box — both from a process nobody is looking at, and both blocking
   until someone clicks OK (in the probe, until the registry's timeout kills
   it). twClapModule::load() therefore wraps LoadLibraryExW in
   SetThreadErrorMode( SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX ) —
   per-thread, so a background scan cannot disturb the host app's error mode —
   and both probe executables call SetErrorMode( … | SEM_NOGPFAULTERRORBOX )
   process-wide in main(), since a probe process exists only to load one module
   and is EXPECTED to die on bad input. Observed, not theoretical: the M2
   verification pass raised three of these boxes on the user's desktop.

13. A TAP NEVER HOLDS ITS OWN COMPONENT MUTEX ACROSS THE SHARED RENDER, and
   pullUpstreamPage() takes it only to SNAPSHOT the producer. The first tap to
   ask for a page renders every bus while holding the PROCESSOR mutex, and it
   gathers the other buses through its sibling taps' pullUpstreamPage(). If a
   tap held its own mutex() across pageFor() -- or if pullUpstreamPage() kept it
   while calling into the producer -- then bus 0 (processor mutex held, wanting
   bus 1's mutex) would deadlock against bus 1 (its own mutex held, wanting the
   processor mutex). So: snapshot the producer shared_ptr under a brief lock,
   RELEASE it, then requestPage(); and do the shared render from renderFrames(),
   which twComponent::freezePage_nolock calls with no component lock held. Lock
   order is always downstream slot -> upstream slot, which is acyclic because
   the chain is. This is exactly the failure class this repo has hit before (the
   input-cursor freeze race, the split-repaint vtable crash), which is why
   plugins_test freezes two taps of one slot CONCURRENTLY -- 120 rounds, with a
   forced re-render each round -- under a 60-second watchdog that aborts loudly
   instead of hanging the suite.
14. Page pulls go through requestPage(), never raw freezePage(), and taps
   inherit the base planPage(). requestPage() is the proposal-19 Phase 2a dedup
   front door: two drivers (revalidation worker, playback readahead, offline
   render) demanding the same producer page collapse to one render. The base
   planPage() gives each tap exactly one grid-aligned dep -- its own bus's
   producer -- which is what the scheduler binds; the OTHER buses are gathered
   inside the render and recorded as plan misses that fall back to the legacy
   pull (correct, just not pre-scheduled). A tap must not override planPage() to
   paper over that.
15. Anything that changes what process() would produce must move the cache key
   AND stale the taps' pages. twPluginSlotProcessor::bumpParamEpoch() does both
   (bypass and state-chunk changes route through it), because there are TWO
   caches in front of a plugin edit: the processor's all-bus page cache and each
   tap's twComponent page cache. Its key is paramEpoch_ plus the SUM of every
   tap's contentEpochNow(); both counters are monotonic, so the sum is too and
   cannot alias -- and including the taps' epochs is what makes an UPSTREAM edit
   (a clip moved) miss the processor cache as well. The app still owns the
   downstream path — and proposal 08 M5 found that it was NOT actually doing it:
   SObject::invalidateRenderPath() called from an SPluginSlot is a no-op, because
   it walks DOWN from the project root through childLinks() looking for `this`
   and an SPluginChain is deliberately not an SLink child of its track. So a
   bypass or parameter edit staled both caches here and still rendered
   byte-identical audio, because the twPluginChain / twTrackMix / mixer pages
   above the taps were untouched. The app side now routes it through
   SPluginSlot::audioInvalidated() -> STrack::onPluginSlotAudioInvalidated() ->
   invalidateRenderPath() on the TRACK (main/pluginui/CONTRACT.md invariant 6,
   gated by qxa.plugin_bypass_and_param).
16. The channel-mismatch mapping is derived ONCE, from the plugin's OWN
   reported layout, and never guessed per page. setBusCount() instantiates one
   plugin first and reads ioLayout() from it, because a descriptor from a stale
   project file or an out-of-date scan cache may disagree. N->N is Direct (one
   instance); 1->1 on N buses is DualMono (N instances -- which is why the
   processor takes an instantiation FACTORY, not one instance); 2->2 on one bus
   is MonoFold (feed both inputs, average the outputs); anything else is
   Unsupported -- the slot loads TRANSPARENT and logs ONCE per slot, never once
   per page. twPluginSlotState { Active, Missing, Unsupported } is declared with
   all three values from M3 so that M4 adds persistence to an existing type; M3
   itself produces Active, Unsupported, and Missing when instantiation fails.
17. A SLOT WITH NO PLUGIN STILL HAS ITS DESCRIPTOR'S GRAPH SHAPE (M4). When the
   factory produces nothing, rebuild_nolock() substitutes
   createNullPlugin( declaredIo_ ) instead of leaving instances_ empty, so the
   channel mapping, the instance count and the prepare() bookkeeping are the ones
   the real plugin will get once it is installed — installing it later changes
   only what process() computes, never the wiring. The slot reports Missing, and
   Missing WINS over Unsupported (a substituted placeholder's layout is whatever
   a possibly-stale project file claimed, so "the plugin is not here" is both the
   cause and the actionable report). Two consequences: a PARTIAL dual-mono chain
   is no longer reachable (the old path cleared every instance and silenced whole
   buses), and the placeholder's own state chunk is EMPTY — so nothing may read
   state from a non-Active slot. SPluginSlot::saveState() enforces that; reading
   it would overwrite the absent plugin's settings with nothing, i.e. a user
   would lose their patch by opening the project on a machine without the plugin
   and saving it.
18. RE-RESOLUTION IS setFactory(), NOT A NEW PROCESSOR (M4). A slot's identity in
   the graph IS its twPluginSlotProcessor: the taps hold it by shared_ptr and
   every twPluginChain holds the taps. So a rescan that finally found the plugin
   hands the SAME processor a new factory, which re-runs exactly what
   setBusCount() derives and stales both caches through bumpParamEpoch_nolock().
   Swapping the processor instead would mean re-wiring every chain of every bus,
   from the UI, for what is not a structural change. The caller re-applies its
   stored state chunk afterwards (SPluginSlot::reloadPlugin).

19. EVERY CONSUMER OWNS ITS OWN twLatchOutput; PLUGS ARE NEVER SHARED. A
   twLatchOutput lives in its producing latch's outputList, and
   twComponent::setInput() disconnects by calling twLatch::deleteOutput() on the
   plug it is replacing. So two components pointed at ONE plug are a trap: the
   first to disconnect unregisters it for both, and because a consumer's
   shared_ptr keeps the object alive the pointer still looks valid — but
   sharedOutput() can no longer vend it, so the next setInput() leaves the input
   NULL and that branch of the graph goes SILENT with nothing logged.
   twPluginChain::rebuildWiring therefore gives the head tap its own
   addOutput() from the producing latch rather than handing over the chain's own
   pInputPlugs_ entry. Removing the head insert used to take the whole chain's
   input with it — a full-silence track, not a wrong level. Gated by
   plugin_order_divergence.qxa, part 3.

20. MODEL SLOT ORDER AND plugins_ ORDER ARE TWO VECTORS, AND EVERY STRUCTURAL
   CHANGE MUST REACH BOTH. A model index is not a plugins_ position; they agree
   only because each of insert / remove / reorder maintains it. Removal is by
   IDENTITY (removePlugin(shared_ptr), never the index overload from the model
   layer), reorder makes the same move in plugins_ (reorderPlugin, not a bare
   rebuildWiring, which re-wires the order it already has), and any path that
   moves a link must go through SPluginChain::reorderSlot so the DSP is told —
   SInsertPluginAction's landing-index move included. When they drifted,
   remove-plugin erased a DIFFERENT insert than the model dropped, silently.
   Gated by plugin_order_divergence.qxa, parts 1 and 2.

How to test: `ctest -R plugins_scan_test` — the scanner gate: cache miss/hit,
invalidate-on-mtime, the stickiness of a failed record (and that force clears
it), cache reload in a fresh registry instance, refusal of a cache from another
scannerVersion, findByUid, rescanAsync + waitForScan, and the same verdicts
through the out-of-process probe. Its "real module" is the twtestclap.clap
fixture and its "bad module" is a file of garbage named *.clap, so nothing has
to be installed. A module that HANGS (the Timeout record) has no cheap fixture;
it shares the probe's kill path and is covered by the manual pass.

Also `ctest -R plugins_test` — the built-in plugin's descriptor /
param / state surface; the M3 channel-mismatch table (Direct / DualMono with
its instance count / MonoFold's average / Unsupported staying transparent); the
M4 missing-placeholder and reload path (a null factory keeping the DECLARED
mapping and staying bit-transparent, setFactory() turning the slot Active in
place with the taps untouched, a parameter applied after the reload being
audible, losing the plugin again falling back, and Missing winning over
Unsupported);
real audio through a two-bus slot (each bus carrying ITS OWN upstream, the two
buses genuinely different, bypass and parameter edits audible on the next
freeze, a preview freeze not re-preparing the plugin); the concurrent two-tap
deadlock gate of invariant 13; plus the real CLAP path (module load, factory,
activate, process, the parameter-event round trip, the state frame's version
tolerance, and 65536-frame pages arriving at the plugin as 4096-frame blocks).
That last one runs against `tests/twtestclap.c`, an in-repo 2-in/2-out CLAP
module built as `twtestclap.clap`: it returns CLAP_PROCESS_ERROR if it is ever
handed more frames than the host declared, and in "report block size" mode
writes the frame count it saw into its output — so a host chunking regression
fails loudly instead of silently. `tools/clap_probe.cc` (target `clap_probe`,
not a gate) loads a real third-party .clap with the production loader and
prints the factory contents. Also qxa.plugin_stereo_chain (a 2-in/2-out CLAP in a
stereo track's signal path, gated on the cross-channel level relation),
qxa.plugin_remove_and_undo (removing a slot really does take the plugin OUT of
the audio, and undo puts it back), qxa.plugin_slot_roundtrip (M4: a SAVED slot
comes back into the signal path with its state chunk applied — 1.5x gain 2.0 =
3x, which discriminates "not in the path" 1.0x from "state ignored" 1.5x),
qxa.plugin_missing_placeholder (M4: an unknown uid loads, sounds transparent, and
re-saves its descriptor and state chunk verbatim) and
qxa.render_sawtooth_with_effects (chain in the signal path); the plugin browser
lists exactly the registry contents.

Known debt:
- THE SINK IS STILL MONO, and that is not M3's doing. The graph carries N buses
  correctly end to end, but both output stages collapse to one page and
  duplicate it: RenderSession ("bufR[i] = sample;  // Duplicate to stereo
  (temporary; proper multi-channel TBD)") and AudioEngine's page pull. Bus 1's
  audio therefore cannot reach a file or a device yet, so proposal 08's "hear it
  in stereo" is blocked by tw_render / tw_playback, NOT by the plugin layer.
  plugin_stereo_chain.qxa works around it with a fixture whose channel 0 depends
  on channel 1's INPUT, which is enough to prove input 1 is wired; per-bus
  DISTINCTNESS is gated at engine level in plugins_test instead.
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
- The processor keeps only twPluginSlotProcessor::kCacheEntries (2) rendered
  pages. That covers what it exists for -- every tap of the slot asking for the
  SAME page -- plus one slot of slack. Two demands for DIFFERENT pages
  alternating will thrash it and, because the plugin is stateful, reset the
  plugin on every position discontinuity. Correct, just slow.
- twPluginChain::calcOutputTo() holds pluginsMutex_ across the whole pull. Safe
  ONLY because the realtime audio callback never renders (twRtThreadGuard); if
  that ever changes it becomes a priority inversion on the audio thread. The
  comment at the lock says so.
- twPluginInsert::renderFrames() learns its page position from seekTo(), which
  twComponent::freezePage_nolock calls immediately before it, inside the
  component's cursorMutex_. Exact, but a side channel: renderFrames' signature
  carries no position.
- A tap always reports a full page of valid frames, exactly as the pre-M3
  insert did. The chain is not the length authority (the render session and the
  project duration are), and a plugin may legitimately produce tail past its
  input -- but it does mean a chain never signals "ran dry".
- CLAP's [main-thread] annotation on activate() is honoured for the common
  case (twPluginInsert prepares in its constructor, which runs on the UI
  thread) but not for a later project sample-rate change, which re-prepares
  from whichever thread renders next.
- Plugin output events (the plugin reporting a moved parameter, gestures) are
  accepted and dropped; no host extensions are offered (get_extension returns
  nullptr for everything). Insert latency is reported, never compensated.
