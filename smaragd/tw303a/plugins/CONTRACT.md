# tw/plugins — CONTRACT

Purpose: the plugin ABI (twPlugin: prepare/process/reset, params, state
blobs), descriptors, the registry, the SCANNER (search paths, on-disk cache,
out-of-process probe), the format backends, and the hosting components
(twPluginChain, twPluginSlotProcessor, twPluginInsert). twPassThrough is the
built-in test plugin (a bit-crusher); twClapPlugin is the CLAP backend
(proposal 08 M1); the scanner is proposal 08 M2; the processor/tap split is
proposal 08 M3; twNullPlugin (createNullPlugin) is the missing-plugin
placeholder of proposal 08 M4; twVst3Plugin is the VST3 backend (proposal 08
M6), which added four files here and changed nothing above the ABI.

Shape of a slot (proposal 08 M3, RESHAPED BY PROPOSAL 36 B4). ONE
twPluginInsert per slot: a twComponent with one port in, one port out and N
CHANNELS, which renders every channel of its page in one process() sweep per
chunk. Behind it one twPluginSlotProcessor (plain C++, not a twComponent) owns
the twPlugin instance(s), the bypass flag, the prepare() state, the block
chunking, the channel-mismatch mapping and the position-continuity reset —
plugin LIFETIME and STATE, not graph machinery. STrack builds ONE twPluginChain
per track, N channels wide, holding one insert per slot in slot order.

  Until B4 the page was one MONO channel, so N channels were N parallel
  component instances and a stereo-linked plugin could not be a component at
  all. The slot was then one processor plus N per-bus TAPS plus a private
  (startPos, len, stamp) -> all-bus page cache: the first tap to ask rendered
  every bus by reaching SIDEWAYS through its siblings. That is what invariant
  13 was written about; both the fan-out and the cache are gone.

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
   twOutputPage::FRAME_CAPACITY (65536) frames PER CHANNEL, which no real
   plugin accepts in one call. twPluginSlotProcessor::kChunkFrames (4096,
   re-exported as twPluginInsert::kChunkFrames) is what prepare() promises and
   what the processor's render actually hands to process(), advancing through
   the same planar buffers so plugin DSP state carries across chunks exactly as
   it would across callbacks in a live host. Since proposal 36 B4 those buffers
   are the CALLER's — the insert's zero-padded gather of its upstream page, and
   its own output page's channels — so the whole page's worth of per-bus
   scratch the processor used to own is gone.
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

13. AN INSERT NEVER HOLDS ITS OWN COMPONENT MUTEX ACROSS A CALL INTO A
   PRODUCER. The rule survives proposal 36 B4; the deadlock it was written for
   does not. Before B4 the first TAP to ask rendered every bus while holding
   the PROCESSOR mutex and gathered the other buses SIDEWAYS through its
   sibling taps, so a tap holding its own mutex() across pageFor() -- or
   keeping it while calling into a producer -- made bus 0 (processor mutex
   held, wanting bus 1's mutex) deadlock against bus 1 (own mutex held, wanting
   the processor mutex). One wide insert has no siblings to gather from, so
   that cycle cannot form. What remains, and is why the rule stays: a
   component's own mutex held across an upstream freeze is a lock-order
   inversion against any concurrent edit, and twComponent::fetchInputPage
   (§4.4 rule 2, the seam the insert reads its upstream page through) is
   written to snapshot the plug under a brief lock and RELEASE it before
   pulling. Lock order is always downstream slot -> upstream slot, which is
   acyclic because the chain is.
   The concurrency gate in plugins_test moved with the hazard: it now freezes
   ONE slot at TWO DIFFERENT POSITIONS from two threads -- 120 rounds, forced
   re-render each round, 60-second watchdog that aborts loudly rather than
   hanging the suite -- which is the race that still exists (two drivers
   demanding different pages of the same component, the "coherent page
   displaced by a whole page" class).
14. AN INSERT INHERITS THE BASE planPage(), AND ITS PLAN IS NOW COMPLETE.
   The base gives it exactly one grid-aligned dep -- its single input plug's
   producer -- which is what the scheduler binds, and since proposal 36 B4 that
   is ALL the render consumes: one upstream page, every channel. Before B4 the
   plan was deliberately incomplete (each tap planned its own bus and the
   OTHER buses were gathered inside the render, recorded as plan misses that
   fell back to the legacy pull -- correct, just not pre-scheduled). An insert
   must still not override planPage(). Reads go through the §4.4 seams --
   fetchInputPage() in the render, requestPage() where a whole page is wanted
   -- never raw freezePage(), so two drivers demanding the same producer page
   still collapse to one render (proposal 19 Phase 2a).
15. Anything that changes what process() would produce must stale the insert's
   pages. twPluginSlotProcessor::bumpParamEpoch() does it (bypass and
   state-chunk changes route through it). Before proposal 36 B4 there were TWO
   caches to move -- the processor's all-bus page cache and the taps'
   twComponent page caches -- and the processor's key had to be paramEpoch_
   plus the SUM of every tap's contentEpochNow() so that an UPSTREAM edit missed
   it too. With one insert there are no siblings to dedup for, the processor
   caches nothing, and the component page cache above it is the only cache
   there is. The app still owns the
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
   reported layout, and never guessed per page. setChannelCount() instantiates
   one plugin first and reads ioLayout() from it, because a descriptor from a
   stale project file or an out-of-date scan cache may disagree. N->N is Direct
   (one instance); 1->1 on N channels is DualMono (N instances -- which is why
   the processor takes an instantiation FACTORY, not one instance); 2->2 on one
   channel is MonoFold (feed both inputs, average the outputs); anything else is
   Unsupported -- the slot loads TRANSPARENT and logs ONCE per slot, never once
   per page.
   PROPOSAL 36 B4 CHANGED THE NUMBER, NOT THE POLICY: it is the PAGE WIDTH the
   insert is handed (the project's channels=), not the count of parallel mono
   components a track was built from. One consequence is audible and is the only
   one in the milestone: a channels='1' project running a 2-in/2-out plugin now
   folds it, where before B4 the project's width reached no track at all and
   every track ran two buses of which the master discarded one. setChannelCount()
   also SHRINKS now -- nothing is created or destroyed by a width change -- so
   re-deriving on a 6 -> 2 undo is ordinary. twPluginSlotState { Active, Missing, Unsupported } is declared with
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
   the graph IS its twPluginSlotProcessor: the insert holds it by shared_ptr and
   the twPluginChain holds the insert. So a rescan that finally found the plugin
   hands the SAME processor a new factory, which re-runs exactly what
   setChannelCount() derives and stales the insert's pages through
   bumpParamEpoch_nolock(). Swapping the processor instead would mean re-wiring
   the chain, from the UI, for what is not a structural change. This invariant
   is also the reason proposal 36 B4 did NOT fold the processor into the insert
   when it retired the tap split: a plugin's lifetime has to outlive any
   particular component. The caller re-applies its
   stored state chunk afterwards (SPluginSlot::reloadPlugin).

19b. AN INSERT IS N CHANNELS WIDE AND MUST KEEP A NARROW renderFrames()
   (proposal 36 B4, §7 trap 18). twComponent's base renderFrames() calls
   calcOutputTo() and its base calcOutputTo() calls renderFrames(), so a
   component that overrides NEITHER recurses until the stack ends -- and a wide
   component IS handed a width-1 page, by the mono-scratch path and by the
   preview path, which no width fork can widen. renderPageWide() and
   renderFrames() therefore share one core with nCh = page.channels() and
   nCh = 1 respectively. The insert also allocates its own pages in freezePage()
   (the ZOMBIE and preview branches), which BYPASSES the width wiring in
   twComponent::freezePage -- both must pass getOutputChannels() explicitly,
   because the render fork is on the width of the PAGE and a declared-N
   component handing itself a width-1 page renders channel 0 and publishes it
   with no refusal at all.

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

21. ON macOS A `.clap` IS EITHER A BUNDLE DIRECTORY OR A FLAT DYLIB, AND THE
   LOADER AND SCANNER MUST AGREE (M7). twClapModule::load() stats the path: a
   regular file is dlopened as-is; a directory resolves the inner binary from
   Contents/MacOS, PREFERRING the bundle base name but falling back to the sole
   regular file there — i.e. CFBundleExecutable without linking CoreFoundation,
   so a bundle whose inner name differs from the bundle base still loads.
   twPluginSearchPaths::bundleBinary() carries the SAME fallback, so the scanner
   discovers exactly what the loader can open; when they disagreed the fixture
   (a flat MODULE dylib named *.clap) loaded in the scanner but not the loader,
   and every macOS plugin test failed. entry_->init() still receives the
   ORIGINAL bundle path, per the CLAP contract, not the resolved inner binary.
   The app also needs com.apple.security.cs.disable-library-validation in its
   entitlements to dlopen an unsigned third-party plug-in under the ad-hoc
   signature — and that plist must stay comment-free (codesign's AMFI parser
   rejects XML comments).

22. THE ONLY ROUTE FROM A VST3 PARAMETER EDIT TO THE DSP IS
   ProcessData::inputParameterChanges (M6). IEditController::setParamNormalized
   updates the CONTROLLER and nothing else — it never reaches IAudioProcessor.
   A host that stops there has a parameter UI that moves and audio that does
   not, which is the single most common VST3 host bug. twVst3Plugin::setParam()
   therefore mirrors the value (what getParam() reads), pushes into the same
   lock-free single-producer ring the CLAP backend uses, and lets process()
   drain it into a pre-sized twVst3ParamChanges; the setParamNormalized call it
   ALSO makes is decoration so a native editor agrees, not the path. Ring
   overflow raises the same resync flag. tests/twtestvst3.cpp deliberately
   IGNORES setParamNormalized, so a regression here fails a level assertion in
   plugins_test rather than going unnoticed.
23. HOST OBJECTS HANDED TO A VST3 PLUGIN ARE EITHER BORROWED OR OWNED, AND THE
   TWO MUST NOT BE CONFLATED (M6). twVst3Borrowed (host context, parameter
   queues, memory streams) belongs to the plugin INSTANCE, lives as long as it,
   and never self-deletes on release() — its refcount is clamped at zero so an
   over-releasing plugin is survivable. twVst3Owned (IMessage, IAttributeList) is
   manufactured on demand through IHostApplication::createInstance, handed over,
   and destroyed when the last release() lands. Those two must be real
   implementations and not stubs: a SPLIT component/controller pair talks to
   itself THROUGH the host, so answering kNotImplemented loads the plugin and
   then silently breaks its internal channel.
24. VST3 STATE IS TWO CHUNKS, AND ONLY A SEPARATE CONTROLLER CONTRIBUTES ONE
   (M6). The frame is 'TWV3' + u16 version + u16 reserved, then two
   length-prefixed chunks (component, controller) — length-prefixed rather than
   "everything after the header" precisely because there are two. The magic
   differs from CLAP's 'TWCP' so a mis-routed blob is REFUSED, not misread, and a
   truncated blob is refused WHOLE rather than half-applied. In a
   single-component plugin IComponent::getState and IEditController::getState are
   the same virtual — identical signatures, so one override serves both and the
   plugin cannot make them differ — hence the controller chunk is written only
   when the controller is a separate object. On load the component chunk must
   ALSO be pushed at the controller via setComponentState, or the editor shows
   defaults over restored audio.
25. A `.vst3` IS A FLAT MODULE OR A PER-ARCHITECTURE BUNDLE, ON EVERY PLATFORM
   (M6). Windows still allows a plain DLL renamed .vst3 (Melodyne ships exactly
   that) as well as Contents/x86_64-win/Foo.vst3; macOS uses Contents/MacOS/Foo
   and Linux Contents/x86_64-linux/Foo.so. twVst3Module::resolveBinary() tries
   the conventional names in the arch dirs and falls back to the sole regular
   file there, and twPluginSearchPaths::bundleBinary() carries the SAME per-format
   arch list — invariant 21's rule generalised, because a loader and a scanner
   that disagree is precisely what broke every macOS plugin test in M7.
   twVst3Module also inherits invariants 11 and 12 verbatim: release a failed
   module OUTSIDE the intern mutex, and wrap LoadLibraryExW in a per-THREAD
   SetThreadErrorMode.
26. VST3 PARAMETERS ARE EXPOSED IN THE NORMALIZED [0,1] DOMAIN (M6).
   twPluginParamInfo carries min 0, max 1, default = defaultNormalizedValue,
   isStepped = stepCount > 0. That IS the VST3 interface domain; converting to
   plain units (dB, Hz) would put an IEditController call on every UI-thread read
   and needs a non-monotonic inverse for stepped parameters, for a slider that
   looks identical either way. Automation is normalized at every other layer too.
   Note the asymmetry with CLAP, whose parameters keep their plugin-declared
   min/max — twPluginParamInfo is expressive enough for both, so nothing above
   the ABI has to care.

27. PARAMETER DISPLAY TEXT COMES FROM THE PLUGIN, NOT THE HOST.
   `twPlugin::paramValueText(id, v)` returns the plugin's own formatting of a
   value (units like dB/Hz/%, enum/choice names, log scaling); empty means "no
   formatter — the host may render numerically". It is DISPLAY-ONLY, UI-thread
   callable, and NEVER on the audio path. `v` is in the same domain getParam()
   returns per format (native for CLAP/AU, normalized [0,1] for VST3) — which is
   why this, not a `unit` field on twPluginParamInfo, is the seam: it consumes
   VST3's normalized value directly rather than denormalizing it (invariant 26),
   and a suffix could not express enum labels or log curves anyway. Backends map
   it to CLAP `params.value_to_text` (a [main-thread] call — no host lock, like
   getParam), VST3 `IEditController::getParamStringByValue` (UI thread; the value
   already normalized), and AU `kAudioUnitProperty_ParameterStringFromValue`. The
   AU backend adds an internal fallback: when the AU implements no
   ParameterStringFromValue it synthesizes "<number><unit>" from the
   `AudioUnitParameterUnit` it stores per parameter (empty for indexed/boolean,
   so the host still formats those numerically). The null/placeholder plugin has
   no parameters, so the default empty override is correct.

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
prints the factory contents.

And the VST3 half (M6): module load through InitDll/GetPluginFactory, the
IComponent / IAudioProcessor / IEditController lifecycle, the normalized
parameter surface, a parameter point reaching the processor through
inputParameterChanges, the two-chunk state frame's version tolerance and its
mutual refusal with a CLAP-framed blob, module interning across two instances,
and descriptor resolution by the 32-hex-digit class id. It runs against
`tests/twtestvst3.cpp`, an in-repo 2-in/2-out VST3 built as `twtestvst3.vst3`,
which deliberately ignores setParamNormalized so invariant 22 has teeth.
`plugins_scan_test` additionally proves `.vst3` is discovered, probed, cached
and resolvable by findByUid, in its own tree so the CLAP counts stay exact.
`tools/vst3_probe.cc` (target `vst3_probe`, not a gate) was the M6 ABI spike and
is kept: it walks a real third-party .vst3 through the whole lifecycle and is
the fastest way to triage "this one plugin will not load" without the app. Also qxa.plugin_stereo_chain (a 2-in/2-out CLAP in a
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
- THE SINK IS STILL MONO, and that is not the plugin layer's doing. The graph
  carries N channels correctly end to end -- since proposal 36 B4 in ONE page
  per component rather than N parallel wires -- but both output stages collapse
  to one page and duplicate it: RenderSession ("bufR[i] = sample;  // Duplicate
  to stereo (temporary; proper multi-channel TBD)") and AudioEngine's page pull.
  Channel 1's audio therefore cannot reach a file or a device yet; that is
  proposal 36 B5. plugin_stereo_chain.qxa still bounds the FILE with a fixture
  whose channel 0 depends on channel 1's INPUT (which proves input 1 is wired),
  and since B4 it ALSO asserts the distinctness directly at the track's root
  component with assert-track-channels -- measured ch0/ch1 = exactly 3, the
  skew fixture's own ratio.
- twPluginInsert::calcOutputTo() -- the streaming pull -- can feed the plugin
  only CHANNEL 0 of its input, because a plug pull is mono by construction
  (§4.4 rule 1) and an insert has one plug. Before B4 it had one plug per bus
  and could gather them all. Nothing in the app reaches that path (every
  consumer of a chain goes through freezePage), and a channel-coherent plugin
  driven from a mono seam has no better answer available, but it is a real
  narrowing rather than a tidy-up.
- CLAP and VST3 are directory-scanned; AU is not, and `.component` is
  deliberately absent from `formatForFile()` in twpluginsearchpaths.cc (AU is
  discovered differently — below). That table stays conservative on purpose: a
  module format we cannot load would be probed, fail, and be cached as a
  PERMANENT failure that a later milestone would have to force-clear. That is
  exactly why `.vst3` stayed unreported until M6 landed the backend, and why the
  entry is gated on TW_HAVE_VST3 so a build without the submodule still cannot
  poison its cache.
- The VST3 backend's SPLIT component/controller path has no automated coverage.
  tests/twtestvst3.cpp is a SINGLE component; the split shape (IConnectionPoint
  pairing, setComponentState, separate controller lifecycle, a non-empty
  controller state chunk) is exercised only against real third-party plugins,
  which no CI machine has. A second fixture class would close it.
- VST3 on macOS and Linux is written but unrun. bundleEntry/ModuleEntry, the
  MacOS and <arch>-linux bundle dirs and dlopen are all in place, but M7's
  lesson was that the flat-vs-bundle split only reveals itself on the platform.
  Expect the .vst3 equivalent of M7's fixture-inside-the-bundle problem:
  twtestvst3.vst3 lands in build/bin, and main/CMakeLists.txt copies
  twtestclap.clap into Contents/MacOS — it will need the same treatment.
- No qxa case inserts a VST3; they all name twtestclap.clap. The model, action
  and serialization layers are format-agnostic and were not touched by M6, so
  this is a coverage gap rather than a risk.
- The VST3 backend offers no host extensions beyond IHostApplication,
  IPlugInterfaceSupport and IComponentHandler, ignores output parameter changes,
  and passes no ProcessContext — so a plugin gets no tempo, no transport state
  and no sample position. Notes (inputEvents) are a documented deferral.
- AudioUnit (macOS, M8) is discovered from the OS component registry, NOT by
  walking directories: `enumerateAuModules()` (twaumodule.cc) lists components
  with `AudioComponentFindNext` and `twPluginRegistry::rescan()` merges them into
  the scan loop as synthetic module keys `au:<type>-<subtype>-<manufacturer>`
  (hex). So an AU "module" is ONE component; a descriptor's `uid` is that triple
  and its `path` is EMPTY — AU instantiates from the component description, never
  a path, which is also why an AU project re-resolves by `uid` and is portable
  without a valid path. `createNullPlugin` still backs a missing AU exactly as
  for CLAP. `SMARAGD_SCAN_AU=0` suppresses AU enumeration for the count-exact
  headless scan gate; it never affects insert/instantiate (those bypass the scan).
- AU state uses the `'TWAU'` frame — the same 8-byte shape as CLAP's `'TWCP'`
  but a DISTINCT magic, so a blob from one backend is rejected by the other
  rather than misread. The payload is the unit's `kAudioUnitProperty_ClassInfo`
  serialized as a binary plist.
- AU test gates are stock-system-AU based (no in-repo `.component` fixture, which
  would need a real `Info.plist`/bundle): `au_test` (unit) skips when no AU is
  registered, and the `au_*.qxa` cases are registered macOS-only. Never assert a
  byte-`cmp` or a tight RMS on a stock AU — its DSP varies by OS version; use a
  QUALITATIVE RMS discriminator (au_effect_audible drives AULowpass to near-total
  attenuation) or a serialization round-trip, never `L != R`.
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
