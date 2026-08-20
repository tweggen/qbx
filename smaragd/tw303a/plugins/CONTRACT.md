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

Proposal 37 P2 added the EVENT half of the ABI (twpluginevents.h: twEventList,
twEventOut, twProcessContext, twPluginCapabilities, twPluginBusInfo), the
event-aware process() overload, capabilities()/audioOutBus*/tailFrames(), the
CLAP/VST3/AU translation behind them, scanner version 2, and twNativeInstrument
(the in-repo 303, format "tw", uid tw.native.303, registered like
twPassThrough). It changed NOTHING in twPluginSlotProcessor, twPluginInsert or
twPluginChain — the hosting components were reshaped by proposal 36-B4 (below).

Proposal 37 P3b made a MIDI clip audible: twPluginSlotProcessor grew the
GENERATOR modes (a 0-input plugin is no longer Unsupported — invariant 16), the
PASS-THROUGH SUM (invariant 37), an event feed read through a twEventSource*
with a per-page collect and a per-chunk rebase (invariant 38), the MIDI -> ABI
value domain (invariant 39), the reset + chase + pre-roll continuity protocol of
design D4 (invariant 40), forgetContinuity() for the P3c run barrier, an
instrument bypass that keeps feeding events (invariant 41), and the rule that an
instrument is FREEZE-PATH ONLY (invariant 42). twPluginChain and twPluginInsert
are UNCHANGED: the head insert always had one input port, and what P3b did was
write down that the processor — not the chain — decides whether the plugin sees
it.

Shape of a slot (proposal 08 M3, RESHAPED BY PROPOSAL 36 B4). ONE
twPluginInsert per slot: a twComponent with one port in, one port out and N
CHANNELS, which renders every channel of its page in one process() sweep per
chunk. Behind it one twPluginSlotProcessor (plain C++, not a twComponent) owns
the twPlugin instance(s), the bypass flag, the prepare() state, the block
chunking, the channel-mismatch mapping, the position-continuity protocol and —
for an instrument — the track's event feed; plugin LIFETIME and STATE, not graph
machinery. STrack builds ONE twPluginChain per track, N channels wide, holding
one insert per slot in slot order. SLOT 0 MAY BE AN INSTRUMENT (proposal 37 P3b,
design D3): the model decides that (STrack::instrumentSlot(), the descriptor's
isInstrument), the processor merely sees a plugin with no audio input and an
event source, and every other slot is an effect exactly as before.

  Until B4 the page was one MONO channel, so N channels were N parallel
  component instances and a stereo-linked plugin could not be a component at
  all. The slot was then one processor plus N per-bus TAPS plus a private
  (startPos, len, stamp) -> all-bus page cache: the first tap to ask rendered
  every bus by reaching SIDEWAYS through its siblings. That is what invariant
  13 was written about; both the fan-out and the cache are gone.

Public headers: twplugin.h, twpluginevents.h, twplugindescriptor.h,
twpluginsearchpaths.h, twpluginchain.h, twplugininsert.h, twpluginslotproc.h.

Depends on: tw/core, tw/graph, tw/events. Forbidden: app headers,
devices/sinks, tw/mix (design F15). tw/events is a CORE-ONLY leaf outside the
dataflow DAG, so the edge adds no page dependency; it exists because there is
exactly ONE twEvent in this codebase and the ABI quotes it (design §4.1).

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
   AMENDED BY PROPOSAL 37 P3b: a GENERATOR gets ONE SORTED EVENT LIST PER CHUNK
   along with the audio. The processor collects the whole page from its
   twEventSource once (times page-relative), then per chunk takes the slice with
   times in [off, off+n), rebases them to 0..n-1 and clamps anything past the end
   into the last frame — never drops it. The UI's setParam ring is NOT merged
   here: each backend already drains its own ring at offset 0 AHEAD of the host
   events (twClapPlugin::drainEditsIntoEvents then appendHostEvents), so the
   plugin still sees exactly one non-decreasing stream and there is only ever one
   ring. A chase set (invariant 40) is the only thing this host inserts itself,
   at offset 0 of the first chunk of a PRE-ROLL.
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
   CONSEQUENCE FOR INSTRUMENTS (proposal 37 P3b): an instrument track's AUDIO
   WAVEFORM PREVIEW IS EMPTY, because the tap forwards its upstream page and an
   instrument's upstream is the track mix, which holds no audio clips. That is
   deliberate: the alternative is rendering a synth at 1 kHz from a redraw. The
   MIDI clip's own thumbnail is app-side (design 6.1), and a level envelope can
   later be read from cached PLAYBACK pages by position, which is proposal 34's
   pattern.
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
   The cache is <configDir>/plugincache.v<kScannerVersion>.json (the app asks
   twPluginRegistry::cacheFileName() for that spelling), NOT twSidecarStore:
   that store keys on a content hash of audio PCM and caps itself with an LRU,
   which would silently evict the plugin table. The version is in the FILE NAME
   as well as in every record because the config dir is shared by every build
   this user runs: one file meant a build at version N and a build at version
   N+1 rejected each other's records on every launch, leaving both permanently
   cold and re-probing every installed plugin in every process.
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
   pages. twPluginSlotProcessor::bumpParamEpoch() does it (bypass, state-chunk
   changes and -- since proposal 37 P5 -- AUTOMATION CURVES route through it).
   THE EPOCH IS THE HASH (design D5): setParamCurves() bumps automationEpoch_
   AND calls bumpParamEpoch_nolock(), so the curve enters the insert's page
   STAMP by way of its content epoch. Post-B4 that is the whole of it -- the
   processor caches nothing, so the component page cache above the insert is the
   only cache there is, and automationEpoch_ is the monotonic counter that says
   which generation of curves a page was frozen against. Before proposal 36 B4 there were TWO
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
   PROPOSAL 37 P3b ADDED THE GENERATOR ROWS. A 0-input plugin used to fall
   straight through to Unsupported, which is why an instrument was inaudible
   rather than merely unsupported. On a C-channel page:

     0 -> C            DirectGen    one instance, channel for channel
     0 -> 1  (C > 1)   MonoSpread   the one voice on every channel (centre-panned
                                    until clips carry a pan and the sink is wide)
     0 -> 2  (C == 1)  GenFold      average the pair down
     0 -> M  (M > C)   WideGen      outs 0..C-1 to the page; the surplus into the
                                    slot's own buffer, which is where design 5.4's
                                    aux taps will read it (P9)
     0 -> M  (1 < M < C)            no defined spread: Unsupported, as before

   The row is chosen from the INSTANCE's ioLayout() like every other row, so a
   descriptor that lies about being an instrument changes nothing about the DSP.
   GATED PER CHANNEL, FROM A RENDERED FILE, SINCE 2026-08-17
   (qxa.instrument_stereo_render): DirectGen (tw.test.clap.sine on channels=2,
   with its new default-OFF `Stereo Skew` param on, so channel 1 is at half
   channel 0 and the two are provably not a duplicate), GenFold (the same
   instrument on channels=1 -- 0.75x, which is what says the PAIR was averaged
   rather than output 0 taken), the REFUSED row (the same on channels=6 ->
   Transparent + Unsupported, silent, in a genuinely six-channel file) and
   MonoSpread (tw.native.303, one output, both channels EQUAL -- the case
   asserts that with an expectReject, because equal channels are the right
   answer for a centre-panned mono voice). WideGen is NOT reachable from a
   script: it needs an instrument with at least three MAIN outputs and no
   in-repo fixture has one (GenFold is tested first, so a 2-out instrument on a
   mono page can never take it), so it stays gated only by
   test_plugin_insert.cc's synthetic 0-in/4-out plugin.
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

28. EVENTS ARE CHUNK-RELATIVE, SORTED, AND ARRIVE AS ONE LIST PER CALL
   (proposal 37 P2). Inside a `twEventList` handed to `process()`,
   `twEvent::time` is 0..nframes-1 of THAT call and never a project position —
   the position of frame 0 is `twProcessContext::position` instead, and the
   context's `validFlags` says which of its fields are real (a host that does
   not know the tempo must SAY so; a plugin cannot tell a real 120 bpm from a
   default one). The list is sorted by time, non-decreasing: CLAP and VST3 both
   require it, so an unsorted list is a host bug that surfaces as a plugin
   refusing to process. Metadata kinds (Tempo, TimeSig, Marker, Lyric, ...) are
   sequence-only and never appear; a backend may assume it never sees one.
   ONE list per call, containing everything — a UI parameter edit, an
   automation slice and the clip's notes are merged by the caller, not
   concatenated by the backend.
29. THE HOST ISSUES NOTE IDS, AND A NoteOff CARRIES THE SAME ID OR -1
   (proposal 37 P2). A note's identity is the id, not (port, channel, key):
   that is what lets two overlapping notes on one key be released
   independently, and it is the only thing per-note expression can target.
   A backend matches an off by id when one was issued and falls back to key
   otherwise; a host that sends a DIFFERENT id on the off leaves the note
   hanging, which is why both sine fixtures match that way and the silence
   assertion in `plugins_test` catches it.
30. THE SAME NOTE IS NEVER SENT IN TWO DIALECTS (proposal 37 P2). CLAP
   negotiates per note port: `clap_note_port_info.supported_dialects` /
   `preferred_dialect`, and we speak CLAP (structured, note ids, note
   expressions) or MIDI 1, exactly one per port, chosen once at instantiation.
   Sending a note as BOTH a `clap_event_note` and a raw MIDI note-on would make
   a plugin that understands both play it twice. A port offering only MPE or
   MIDI 2 receives nothing and says so once in the log, rather than being sent a
   dialect it never asked for. The host now vends `clap.host-note-ports`,
   `clap.host-params` and `clap.host-tail`; all three only RECORD (the audio and
   worker sides must never reach into component wiring, and twComponent is not
   a QObject to signal from).
31. THE VST3 kEvent BUS MUST BE ACTIVATED AT prepare(), AND UNTIL PROPOSAL 36
   P2 IT WAS NOT. `twVst3Plugin::prepare` switched on the AUDIO buses only, so
   a plugin that gates its note handling on `activateBus` — which the spec
   entitles it to do — received a perfectly well-formed `IEventList` and ignored
   every note in it. The symptom is total silence from an instrument with NO
   error anywhere. `tests/twtestvst3.cpp`'s `TestSine` reproduces it
   deliberately (it ignores an unactivated bus), and `SMARAGD_VST3_NO_EVENT_BUS=1`
   suppresses the activation so `plugins_test` can drive the SAME fixture down
   the broken path and watch it go silent — an assertion that a bug is fixed is
   worthless unless it can still fail. Never set that variable in production.
   Two more VST3 facts of the same kind: a CONTROL CHANGE has no VST3 event type
   at all, so `IMidiMapping::getMidiControllerAssignment` (queried once at
   prepare, on the CONTROLLER) is the ONLY route a CC has to the DSP and an
   unmapped CC is DROPPED rather than assigned a parameter we invented; and
   parameter points are added at their own `sampleOffset`, which is what makes a
   mid-block automation step land on the right frame.
32. EVENT STORAGE IS SIZED IN prepare() AND OVERFLOW IS COUNTED, NOT GROWN
   (proposal 37 P2, an instance of invariant 2). `twEventLimits::kMaxEventsPerBlock`
   is what a host may send and what a backend reserves — the CLAP event vector,
   the VST3 `twVst3EventList` pair and the parameter queues' per-parameter point
   capacity. A plugin that pushes more into `twEventOut` than the host sized for
   loses the surplus and the host can read `dropped()`; growing would allocate
   on the render path and returning an error would make a chatty arpeggiator
   fail a render.
33. THE LEGACY process() IS THE SAME CODE, NOT AN EQUIVALENT ONE (proposal 37
   P2). Every backend's three-argument `process()` forwards to the event-aware
   overload with an EMPTY list, an unreachable sink and an all-invalid context —
   so no host events are translated, `clap_process::transport` and
   `ProcessData::processContext` stay nullptr, and output events are still
   discarded. That identity is what lets the effect goldens be compared byte for
   byte across this phase, and it is why a backend must never override BOTH
   overloads with independent implementations. `acceptsNotes()` likewise stays
   as a forwarder to `capabilities().acceptsNotes` for one release; a backend
   overrides `capabilities()` and gets it for free.
34. AUX OUTPUT BUSES ARE DISCOVERED AND NOT YET ROUTED (proposal 37 P2).
   `audioOutBusCount()` / `audioOutBus(i)` report every audio output bus — CLAP
   ports, VST3 buses, AU output ELEMENTS — and the scanner records them
   (`nOutBuses`, `outBusChannels`). Only bus 0 is wired: the event-aware
   `process()` reads `outBuses[0]` and nothing consumes the rest. Proposal 37
   §5.4 routes them to return tracks in P9; reporting them now is what stops
   that from needing an ABI change.
35. AU IS macOS-ONLY AND ITS EVENT PATH IS UNVERIFIED (proposal 37 P2). The
   `aumu`/`aumi` enumeration, `MusicDeviceMIDIEvent` posted BEFORE
   `AudioUnitRender` with its own `inOffsetSampleFrame`,
   `AudioUnitScheduleParameters` for sample-accurate parameter steps, and the
   output-element walk are all written to the documented API — and were written
   on Windows, where the whole backend is compiled out. Nothing in it has ever
   been compiled, let alone run against an AudioUnit. `plugins_test` says so
   out loud on a non-Apple build rather than leaving a silent gap. AU MIDI-OUT
   is reported as a capability but NOT wired: `kAudioUnitProperty_MIDIOutputCallback`
   must be installed BEFORE `AudioUnitInitialize`, which is a lifecycle change
   this phase did not make.

37. THE HEAD INSERT ALWAYS HAS ONE AUDIO INPUT; THE PROCESSOR DECIDES WHETHER
   THE PLUGIN SEES IT (proposal 37 P3b, design D3). twPluginChain's head-input
   wiring is unconditional — it does not ask what slot 0 is — and for a
   GENERATOR the processor does not hand that input to the plugin at all: it
   ADDS it to the plugin's output ("the pass-through sum"). That is what keeps
   an audio clip on an instrument track audible with no track kind, no second
   graph shape and no change to any invalidation walk.
   `x + 0.0f == x`, so an instrument with NO NOTES leaves the render BYTE-
   IDENTICAL to the render with no instrument at all — which is the sharp gate
   (qxa.instrument_mixed_track) and the reason the sum is stated as an
   invariant rather than left as an implementation detail. The sum is centre-
   panned mono into a stereo instrument's outputs until clips carry channels
   (design D3, accepted and stated). What an instrument's OWN outputs do across
   the channels has been gated since 2026-08-17 by qxa.instrument_stereo_render;
   the pass-through sum's centre-mono spread is what is still not, because a
   clip carries no pan to spread it with.

38. AN INSTRUMENT READS A twEventSource*, AND NEVER THE MODEL (proposal 37 P3b).
   The processor holds a shared_ptr<const twEventSource> swapped under mutex_,
   so a render already holding one keeps a live source for its whole duration.
   The APP sets it (SPluginSlot::setEventSource <- STrack::syncInstrumentSlot)
   and the APP refreshes what the merge CONTAINS on the main thread
   (STrack::eventFeed(), driven from bumpRenderChainEpoch/Range — every model
   change that reaches the track passes through there). Nothing in tw/plugins
   walks childLinks(), resolves solo, or touches Qt. The source is a track's
   FEED (its own clip set merged with the feeds of the children that bubble up,
   design 3.2.1) and the processor cannot tell a merge from a plain clip set.

39. THE FEED SPEAKS MIDI, THE ABI SPEAKS [0,1], AND twNormalizeForAbi() IS THE
   ONLY PLACE THEY MEET (proposal 37 P3b). tw/events is MODEL data: SMidiSequence
   stores `add-note velocity='100'` verbatim, SMidiOutPump sends clamp7(e.value)
   onto the wire, and the piano roll draws value/127. The plugin ABI is
   normalized, because CLAP and VST3 both are (and twNativeInstrument's accent
   threshold is 100/127). So the processor divides note velocity, CC,
   poly/channel pressure by 127 and pitch bend by 8192 on the way into a chunk
   list — and leaves ProgramChange (an index) and ParamValue (already the
   plugin's own domain, invariant 26) alone. Normalizing in tw/events instead
   would force the pump to multiply back up and would round-trip a project
   through a lossy scale.

40. A GENERATOR PAGE THAT IS NOT CONTIGUOUS IS A REPOSITION: reset + chase +
   PRE-ROLL, NOT A BARE RESET (proposal 37 P3b, design D4). An effect resets and
   carries on; an instrument cannot, because the note that is sounding at P had
   its note-on pages ago. So a page whose startPos is not lastEnd_ runs:
   reset() (all notes off) -> chase stateAt(P-K) as events at offset 0 (held
   notes with their velocities, plus every controller value that got them there)
   -> K frames rendered with the events at their real offsets and the OUTPUT
   DISCARDED -> then the page.

       K = min( max(4096, tailFrames(), P - start(earliest note held at P)), 4 s )

   K reaches back to the HELD NOTES because that is the only way an envelope
   arrives at P in the state a continuous run would have given it; the 4 s cap
   bounds the cost, and a note held longer than that has converged. K is also
   clamped to P — there is nothing before frame 0 to pre-roll.
   TWO CONSEQUENCES WORTH KNOWING. (a) The page render NEVER re-issues the
   page's own chase set: it has just been rebuilt into the DSP, and re-attacking
   it would double every held note. The chase is consumed by the PRE-ROLL only.
   (b) Every generator page is therefore a PURE FUNCTION of its position and the
   feed, which is why an instrument re-render is byte-identical and why
   qxa.instrument_edit_reaches_render can byte-compare the untouched region.
   An epoch bump does NOT clear lastEnd_ — only rebuild_nolock(), a rate change
   and forgetContinuity() do. That is exactly why the P3c run barrier has to
   call forgetContinuity() explicitly: a render whose first page starts where the
   previous run stopped would otherwise CONTINUE that run's voices.

41. AN INSTRUMENT BYPASS IS SILENCE, NOT A SHORT CIRCUIT (proposal 37 P3b). An
   effect's bypass copies the input and skips process(). A generator's must NOT:
   the events would never arrive, the note-offs inside the bypassed span would
   be lost, and un-bypassing would resurrect voices that should long since have
   ended. So process() is still called with every event and the AUDIO is
   discarded; the pass-through sum still runs, so the track's own clips stay
   audible through a bypassed instrument.
   Gated in plugins_test (testGeneratorSlot), NOT in a qxa case, and that is
   structural rather than laziness: the difference is only observable when the
   flag moves between two CONTIGUOUS page renders of one run, and a script
   cannot express that — a render always starts at the range start and every
   non-contiguous page is a reposition (invariant 40), which rebuilds the voices
   from the feed whatever the bypass history was. There is no in-app automation
   of a bypass until proposal 37 P5.

42. AN INSTRUMENT IS FREEZE-PATH ONLY (proposal 37 P3b, design 4.3).
   twPluginSlotProcessor::render( positional = false ) — the legacy streaming
   pull, twPluginInsert::calcOutputTo — has no page identity and therefore no
   position, so it cannot place a single event. A generator answers SILENCE
   there and logs once per slot. The consequence is deliberate and is written
   down in main/testkit/CONTRACT.md as well: SMARAGD_REVAL_WORKERS=0 (legacy
   pull everywhere) makes instrument tracks silent BY DESIGN, so an instrument
   race sweep runs over workers {1,4,8,16} and never 0. The legacy pull is on
   proposal 20's retirement list.

36. THE SCAN IS STOPPED FROM THE APP'S ORDERLY TEARDOWN, NOT FROM A
   DESTRUCTOR. twPluginRegistry::stopScan() sets a flag the scan loop reads
   BETWEEN two modules and then joins the thread; SApplication's destructor and
   main.cpp's smaragdOrderlyShutdown both call it, so the scan thread is gone
   before static destruction begins. It has to be explicit because a
   --test-case run leaves through std::exit(), where no stack object is
   destroyed at all: the registry is a namespace-scope static, ~twPluginRegistry
   then joined a scan thread that was still LOGGING, and with a mortal log sink
   (destroyed earlier, because it was constructed later) that thread abort()ed
   and deadlocked the process after PASS had been printed — reproduced 6 hangs
   and 3 crashes in 10 cold-cache runs, plan/STATE.md 2026-08-16. The abort
   point is deliberately between modules, never inside a probe, so the join is
   bounded by one probeTimeoutMs_ at worst. An aborted scan still SAVES the
   cache — the records it probed plus, carried over, the records for modules it
   never reached — so successive short runs converge instead of restarting cold
   forever (invariant 9's stickiness is preserved either way). It does NOT
   replace plugins_: a partial result is not the plugin table.

43. NO SHUTDOWN PATH MAY WAIT ON THE SCAN UNBOUNDED, AND NOTHING MAY ESCAPE THE
   SCAN THREAD. Invariant 36 says WHO stops the scan; this says what the stop is
   allowed to cost and what it must survive. `requestStopScan()` sets the flag;
   the scan reads it in THREE places, not one -- between modules, once per 100 ms
   slice of the out-of-process probe wait (the child is KILLED, not orphaned),
   and immediately before the in-process fallback, which cannot be interrupted
   once it has entered a plugin's DSO. Between-modules alone bounds a stop by one
   `probeTimeoutMs_` PER installed module, which is not a teardown budget.
   `stopScan( ms )` then joins with a TIMEOUT: an expired join LEAKS the worker
   rather than deleting it (undefined) or terminating it (worse -- it may be
   inside a plugin), KEEPS the pointer so a later join retries the same thread,
   does NOT clear the stop request (the worker must keep seeing it), and LOGS
   loudly. Unbounded is not an option for the same reason invariant 36 exists:
   the worker may be unable to return at all, having already died on an exception
   and wedged in `abort()`. Which is the last part -- the thread body carries a
   CATCH-ALL. An exception out of a QThread reaches `std::terminate`, whose
   `abort()` blocks on the CRT lock an exiting thread already holds while it
   waits for this one, and that deadlock has no timeout in it. Invariant 36
   removed the KNOWN exception (a destroyed log sink); this removes the class.
   Reporting the caught exception is itself wrapped, because the reason we got
   there may be that reporting is what fails. An aborted scan is asserted usable
   in `plugins_scan_test`: the cache still parses, and the next `rescanAsync()`
   probes everything and completes.

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
and resolvable by findByUid, in its own tree so the CLAP counts stay exact, and
(proposal 37 P2) that a scanner-VERSION-1 cache is discarded and every module
re-probed exactly ONCE, that the probe's JSON carries the new descriptor fields
for all three test modules, and that those fields survive the plugincache.json
round trip.

And the EVENT half (proposal 37 P2), driven DIRECTLY on `twPlugin::process` by a
small block pump in `plugins_test` — 4096-frame calls with a chunk-relative
event list, no processor and no tap anywhere near it, because P2 changes the ABI
and the backends and nothing about the hosting components. Per format (the
native 303, `tw.test.clap.sine`, the SPLIT VST3 `TestSine`): a NoteOn(60, vel
100) at offset 1000 gives EXACT silence before it (peak < 1e-6), a fundamental
of 261.6 +/- 1 Hz by autocorrelation with parabolic interpolation over 4096
frames (integer lags alone resolve only to ~0.8 Hz at this pitch, which would
sit on the band), an RMS of vel/sqrt(2) +/- 2 % for the sines, and EXACT silence
after the NoteOff at 30000 — for the 303, 512 frames later, once its 6 ms VCA
release has run out. Plus: a `ParamValue` at block offset 1234 stepping the
level at EXACTLY frame 1234 (CLAP and VST3 — and the VST3 fixture ignores
`setParamNormalized`, so only a correctly-offset `inputParameterChanges` point
passes); the unactivated-event-bus teeth of invariant 31; `tw.test.clap.arp`'s
note-out count against its closed form (ceil(N/4096) ons, each paired with one
off, over 65536 frames); and reset determinism — reset, NoteOn at 0, 8192
frames, twice, byte-identical, for all three. AU is SKIPPED on a non-Apple build
and says so (invariant 35).

The in-repo fixtures grew to match: `twtestclap.c` exports four plugins now
(`gain` with a third parameter, id 2 `Clip Threshold`, which hard-clips AFTER
the gain and is the order-sensitive fixture proposal 37 P3a's fader-move case
needs; `stereoskew`; the `sine` instrument with a stereo main out AND a mono aux
out, carrying since 2026-08-17 a `Stereo Skew` at param id 3 -- stepped,
DEFAULT OFF, so every render made before it is byte-identical; ON, channels 1..
of the main bus are at half amplitude, which is the closed form
qxa.instrument_stereo_render needs to tell a wide sink from a duplicating one;
and the `arp`), and `twtestvst3.cpp` exports the `TestSine` SPLIT
component/controller pair, which closes the "split VST3 pair untested" debt this
file carried since M6. `tw.test.clap.gain`'s state blob still writes 16 bytes
when the clipper is off, so `plugin_slot_roundtrip.qxa`'s exact-base64
assertion is untouched; the third double is appended only when it is set.
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

THE SINK IS NO LONGER MONO (proposal 36 B5), and the entry that stood here for
five milestones is retired. It said: the graph carries N channels but both
output stages collapse to one page and duplicate it, so channel 1's audio cannot
reach a file or a device, and therefore a rendered WAV's two channels are equal
BY CONSTRUCTION and `L != R` must never be asserted on a file. All of that is
now false. RenderSession interleaves from one wide root page into a file of the
project's width, and AudioEngine::pullBlock fills N planar buffers.

  Proposal 37's instrument and automation phases (P3b/P5) were written in the
  same mono-sink era and carried the same caveat -- "channel 0 only, never
  L != R". RETIRED 2026-08-17 by qxa.instrument_stereo_render (the generator
  mapping rows above, per channel) and qxa.automation_stereo (a self:Volume
  ramp and a per-chunk param: step, both channels).

  plugin_stereo_chain.qxa consequently bounds CHANNEL 1 OF THE FILE at
  [0.030, 0.037] -- 0.5x, the skew fixture's own second output -- against
  channel 0's 1.5x, which is the assertion that case documented as its future
  from M3 onward. It also keeps B4's assert-track-channels assertion at the
  track's root component: ch0/ch1 = exactly 3, the same ratio one stage
  earlier. Both are worth having; a file assertion and a page assertion fail
  for different reasons.

Known debt:
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
- The VST3 SPLIT component/controller path is covered since proposal 37 P2:
  `TestSine` in tests/twtestvst3.cpp is a real split pair (IConnectionPoint
  pairing, getControllerClassId, setComponentState, a separate controller
  lifecycle and its own state chunk), and `plugins_test` drives it. What is
  still uncovered is a split plugin that USES the message channel between the
  two halves for something functional — ours connects and answers, but never
  sends.
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
  attenuation) or a serialization round-trip. (`L != R` on a file is legitimate
  since proposal 36 B5, but it says nothing about a stock AU's DSP, which is what
  this bullet is about.)
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
- A GENERATOR PRE-ROLLS ON EVERY REPOSITION, and the scheduler currently renders
  each instrument page twice under a render (once per demand path), so a page at
  P with a note held since 0 pays 2 x P frames of discarded DSP. Correct and
  deterministic — every page is a pure function of its position (invariant 40) —
  but it is O(P) per page rather than O(1), and it is the first thing to look at
  if an instrument render is slow. The obvious fix (keep the pre-rolled state
  when the NEXT page turns out to be contiguous) is what lastEnd_ already does;
  what is missing is de-duplicating the double demand.
- twProcessContext::playing is always true on the freeze path. A freeze
  represents a moving timeline whether it came from playback or an offline
  render, and there is no third state to report; a plugin that wanted "stopped"
  has nothing to distinguish it from a readahead page.
- The WideGen surplus channels are rendered and DROPPED (invariant 16). They are
  design 5.4's aux outputs and there is no return track to route them to until
  P9; the cost is one chunk buffer per surplus channel.

## Parameter automation (proposal 37 P5, design D5 / §4.5)

41. **A `param:` LANE BECOMES PER-CHUNK, SAMPLE-OFFSET `ParamValue` EVENTS, NOT
    A setParam() CALL.** `setParamCurves(map<paramId, curve>)` swaps the whole
    map under `mutex_`; `buildAutomationChunk_nolock(chunkStart, n)` produces
    ONE sorted list per 4096-frame chunk:

      * the value AT the chunk start, at offset 0 — the **chase**. Pages freeze
        out of order and on any worker, so what the plugin instance currently
        holds is unknown BY CONSTRUCTION and is stated rather than assumed;
      * one event per breakpoint strictly inside the chunk, at its own offset;
      * a `kAutoRampFrames` (64) grid along CONTINUOUS segments, for the many
        plugins that do not interpolate between parameter points.

    A value equal to the last one emitted is skipped, so a STEP lane costs two
    events per chunk rather than sixty-four, and the whole list is capped at
    `twEventLimits::kMaxEventsPerBlock`.

42. **THE CURVE-ABSENT PATH IS THE LEGACY CALL, byte for byte.** With no curves
    (and on the positionless legacy pull, which cannot place an event at all)
    `runChunked_nolock` makes the same three-argument `process()` call it always
    made — the same code, not an equivalent one. That is what keeps every render
    without a lane byte-identical (P5 AC6). Only when a curve exists does it
    switch to the event-aware overload.

43. **ONE LANE AUTOMATES THE SLOT, NOT A CHANNEL.** Under `DualMono` every
    instance is handed the SAME event list, exactly as `set-plugin-param` writes
    every instance. The generator path merges the automation events into the
    same per-chunk list as the notes and `stable_sort`s the result: the chase,
    the automation grid and the window's events are three ordered streams and
    only a final sort makes the non-decreasing sequence every backend requires.

44. **THE VALUE DOMAIN IS THE PLUGIN'S HOST-FACING ONE** — native for CLAP/AU,
    normalized [0,1] for VST3 (invariant 26), i.e. exactly what
    `set-plugin-param` writes and `getParam()` returns. `twNormalizeForAbi()`
    deliberately leaves `ParamValue` alone for the same reason.

45. **LIVE OWNERSHIP IS A PROTOCOL, AND EVERY PART OF IT IS FLAG-GATED**
    (proposal 21 L1a, design D2/D4). While `setLiveOwned(true)`, the processor
    belongs to ONE thread — the `LiveGraphPump`, identified by the per-thread
    render-policy marker `twRtThreadGuard::onLiveThread()`, not by a thread id
    of its own. A `render()` arriving from any other thread answers SILENCE and
    bumps the process-wide `liveOwnedRefusals()` counter, with one log line per
    slot. Deliberately not an assert: the design says such an arrival is
    RECOVERABLE (a preview or an asset capture on the revalidation lane is not
    a graph node and cannot be retired), so it must be a number
    `assert-render-policy` can bound, never a process the user loses.

    A refusal does NOT touch `lastEnd_`/`haveLastEnd_`. The refused caller is
    the foreign one; clearing continuity would make the PUMP's next block a
    spurious reposition.

    `setLiveOwned()` itself DOES clear continuity in both directions: the two
    owners render different position streams, so the first block of the new
    owner is one reposition by construction. Handing ownership back also drops
    the live event source and resets the transport.

46. **THE PROCESSOR HOLDS TWO EVENT SOURCES.** `events_` is THE FEED
    (`setEventSource`, owned by `STrack::syncInstrumentSlot()`, also read by
    `SMidiOutPump` and `assert-midi-events`); `liveEvents_` is the LIVE source
    (`setLiveEventSource`), collected ONLY while live-owned and merged into the
    same block with its note ids namespaced to `kLiveNoteIdSource` (15). It is
    not a `setEventSource` swap — that would be overwritten by the next
    `syncInstrumentSlot()` and would clear continuity and bump the param epoch
    per call — and it is not a member of `eventFeed()`, because a ring-draining
    `collect` has exactly one legal reader.

    KNOWN DEBT: a feed built from SIXTEEN merged children would reuse index 15
    and could collide. Ids only have to be distinct within one collect and the
    failure mode is one truncated note, so this is recorded rather than papered
    over with a wider id.

47. **`twLiveTransport` IS CONSULTED PER CHUNK, AND ONLY WHILE LIVE-OWNED.**
    Three fields, all inert otherwise, so the frozen path — and the golden
    corpus — is byte-identical:
      * `playing` becomes `twProcessContext.playing`. Every freeze path still
        reports `true` (a render IS a moving timeline to a plugin's transport);
        a live-owned slot under a stopped transport is the first caller for
        which that would be a lie.
      * `feedEnabled == false` skips collecting `events_` **and skips the
        pre-roll**, which chases and replays the same feed — running it with the
        feed masked would put the sequenced material into the DSP by the back
        door. `liveEvents_` is always collected.
      * `holdAutomationAt >= 0` makes the per-chunk build
        `buildAutomationChunk_nolock(holdAt, 1)`: the chase alone, one constant
        value for the whole chunk. No `setParamCurves` change is needed to
        express "held", and a one-frame window cannot contain a breakpoint or a
        ramp point, so the constancy is by construction.

## Native editors (proposal 33)

`twPluginEditor` (`tw/plugins/twplugineditor.h`) is the format-neutral ABI a
plugin's OWN interface crosses. `twVst3Editor` (M3) and `twClapEditor` (M6)
implement it; both live in `plugins/src/` for the reason invariant 4 gives — a
public header naming `IPlugView` or `clap_plugin_gui_t` would change shape with
`TW_HAVE_VST3` / `TW_HAVE_CLAP` and give this library and its consumers
different views of the same type.

48. **A GUI EDIT'S PRE-EDIT VALUE IS PART OF THE EDIT.**
    `twEditorParamEdit::previousValue` is filled by the backend one instruction
    before it overwrites the mirror, because that is the last moment the old
    value exists anywhere. `poll()` applies each Change to the mirror and the
    DSP on its way through — the whole point, so a knob is audible whatever the
    host does — which means a host that asks `getParam()` afterwards gets the
    NEW value and builds an inverse that restores what it just set. Undo becomes
    a silent no-op. That shipped in M5 and `qxa.plugin_native_editor` caught it.

49. **CLAP EDITS ARRIVE THROUGH `out_events`, ON WHATEVER THREAD CALLED
    `process()`, AND `guiMutex_` IS WHAT MAKES THE OTHER ROUTE LEGAL.** Unlike
    VST3's `performEdit` (a UI-thread callback), a CLAP plugin reports a GUI
    edit as `CLAP_EVENT_PARAM_VALUE` / `PARAM_GESTURE_BEGIN` / `_END` in
    `clap_process::out_events`, i.e. from a revalidation worker — or, when
    nothing is rendering, only after the host answers
    `clap_host_params->request_flush` by calling `params->flush()`.

    CLAP forbids `flush()` concurrent with `process()` and annotates it
    `[active ? audio-thread : main-thread]`. This engine freezes pages on demand
    on worker threads and keeps an instance ACTIVE from the first `prepare()`
    until teardown, so "call it from the audio thread" is not a schedule the
    host can keep — and without the flush, a knob turned with the transport
    stopped (the commonest case there is) never reaches the model at all.
    `twClapPlugin::guiMutex_` supplies the property the spec actually asks for,
    non-concurrency, instead of the thread it suggests as the usual way to get
    it: `process()` takes it unconditionally, `guiDrain()` takes it with
    `try_lock` and comes back on the next 30 Hz tick. A worker therefore waits
    at most one `flush()` call and the GUI never waits at all. It is taken
    unconditionally rather than only while a window is open because "is a window
    open" cannot be tested without racing a `process()` already in flight.

    Consequences a change must preserve: `outEventsTryPush()` appends to
    `guiEdits_` with NO lock of its own (its callers already hold `guiMutex_`),
    and the five `clap_host_gui` callbacks touch ONLY atomics — they are
    `[thread-safe]` by annotation and the main thread holds `guiMutex_` while
    inside calls that can answer with one of them, so taking it there would
    deadlock on a non-recursive mutex.

50. **THE OUT-EVENT CAPTURE IS GATED ON `liveEditors_`.** With no window open,
    not one instruction of it runs on the render path — which is what keeps
    every golden and every pre-M6 measurement exactly as it was. The same
    counter is what lets the destructor report the contract violation of an
    editor outliving its plugin, and what refuses a second editor on one
    instance (`clap_plugin_gui` has no notion of two views: `create()` and
    `destroy()` take no handle).

51. **A CLAP EDITOR NEEDS NO MIRROR-TO-DSP PUSH, AND MUST NOT MAKE ONE.** The
    GUI and the DSP are ONE instance, so the audio has already followed; all the
    host owes is the mirror, so `getParam()` does not disagree with what is
    playing. VST3 is the opposite case — controller and processor are separate
    and `inputParameterChanges` is the only route — which is why
    `twVst3Plugin::applyGuiEdit` pushes the ring and the CLAP path does not.
