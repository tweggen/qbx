# app/shell — CONTRACT

Purpose: the composition root — SApplication (singleton: project lifecycle,
speaker + PlaybackContext implementation, locator authority, selection
list, action submission, sessions), SMainWindow (menus, transport, window
layout), SSettings (per-user INI), main().

Public headers: app/shell/*.h

Depends on: everything (the ONLY module allowed to). Since Phase 6 the
CORE modules (model, actions, persistence, selection, objects/*) are
shell-free: they reach the app exclusively through SAppContext
(app/model/sappcontext.h), which SApplication implements (setInstance in
the ctor, before any project exists). Keep that interface MINIMAL — every
method added is coupling handed to everything below the shell. Remaining
shell edges (timeline/pluginui/servicesui/testkit) are the UI layer and
legitimate.

Invariants:
1. setGlobalLocatorPosRealtime is an ATOMIC STORE ONLY — no signal, no
   QObject machinery (audio/worker threads call it; THREADING.md rule 1).
   The UI playhead is driven by the pumpLocator QTimer.
2. PlaybackContext implementation: rootComponent()/locatorPosition() UI
   thread; locatorHeldElsewhere()/publishPosition() audio thread, atomics.
3. Session wiring (render/record onPosition, startLocatorFrames, speaker
   context) happens HERE — engine modules never see the app.
4. Window layout restore order: openMostRecent() → restoreWindowLayout() →
   show() (see STATE.md 2026-07-11 — reordering re-breaks startup).
5. Startup: settings-selected device is applied to the speaker before any
   playback.
6. Metering pump (proposal 34): `meterTimer_` (33 ms) emits `meterTick(pos,
   nowMs, live)` to every meter. `pos` is LATENCY-COMPENSATED here, once, so
   all meters agree with each other and with the ear —
   `meterLatencyFrames()` scales the backend's `outputLatencyFrames` from
   DEVICE frames at the device rate into PROJECT frames (skipping that
   conversion mis-compensates by the rate ratio). It is a SEPARATE timer from
   `locatorTimer_`, not a fold into `pumpLocator`: meters need a tick at an
   unchanged position (to decay) plus a tail after stop, or the bars freeze
   mid-level. It does NOT run during an offline render (which publishes
   positions faster than realtime) but DOES run while recording (monitoring
   playback is live). Consequence, deliberate: meters trail the DRAWN playhead
   by the device latency, because the playhead itself is uncompensated.
7. MIDI-out pump (proposal 37 P7b): `SMidiOutPump`, a 20 ms main-thread
   QTimer started by `setPlaying(true)` and stopped by `setPlaying(false)`.
   It reads the playhead, slices every MIDI-out track's `STrack::eventFeed()`
   over a 250 ms lookahead, and enqueues into a per-port `MidiOutScheduler`.
   Load-bearing details, each of which is a bug if changed carelessly:
   - It is NOT a fold into the metering pump. The meters keep ticking after a
     stop so the bars can decay; MIDI-out must go silent — and send its
     all-notes-off — at the instant the transport does.
   - A RENDER never starts it (`startRender` does not set `isPlaying_`) and
     `tick()` returns immediately while one is active. This is the proposal-34
     lesson verbatim: a freeze-time MIDI-out would spray a whole arrangement at
     the user's hardware with the transport stopped.
   - The clock ANCHOR is re-taken on every position PUBLICATION
     (`locatorPublishSeq`), not on every position CHANGE. twSpeaker defers the
     device start until the readahead is primed, so before the first callback
     the playhead sits still at the locator and a due time hung on it fires
     before a single frame has been delivered — measured, and it put the first
     note 59 ms early.
   - Two corrections that are properties of the DEVICE, not of the pump: the
     PUBLISH LAG (twSpeaker publishes `currentPosition()` AFTER the pull, so the
     frame just delivered is `published - bufferFrames`) and the OUTPUT LATENCY
     (`meterLatencyFrames()`, reused verbatim because it already converts device
     frames at the device rate into project frames).
   - De-dup is a monotone per-track FRONTIER plus its loop ITERATION, which
     subsumes the `(clip key, event ordinal, loop iteration)` key of the design
     and additionally survives an edit that renumbers ordinals mid-flight.
   - The enumeration probe ports are constructed WITH the pump, before any
     scheduler's port, and never destroyed: `CaptureMidiOutput` registers the
     most recently constructed instance as the one the testkit reads.
   - The scheduler threads are joined in `~SApplication`, on the main thread,
     while the log sink is alive — not during static destruction.
8. `SSettings` owns the machine-local half of a MIDI port: a project stores a
   portable port NAME (`STrack::midiOutPort`), `midi/portId/<name>` maps it to
   the backend id `open()` wants. Unmapped falls back to matching the backend's
   own id/name list, then to the name verbatim.


7. **Docks are created in the CONSTRUCTOR, with a stable objectName**, or
   `restoreWindowLayout()` cannot restore them (inv. 4). There are now six:
   `dock_extern_file_list`, `dock_track_detail`, `dock_log`,
   `dock_clip_properties`, and — since proposal 37 P4 —
   `dock_event_editor` (bottom, tabified with the Log) and
   `dock_virtual_keyboard` (bottom, tabified with the editor). Both start
   hidden; the View menu carries their toggles (Ctrl+Shift+E for the editor).
8. **The event editor's time axis is linked to the arranger HERE**
   (`linkEventEditorAxis()`), because the shell is the only module that sees
   both app/timeline and app/eventui — the editor deliberately has no
   dependency on the 4000-line arranger. The two connections are kept in
   members so a re-link (a new project, a testkit call) REPLACES them instead
   of stacking a second lambda on the same signal.
9. The P4 test seams (`describeTrackHead`, `describeEventEditor`,
   `grabEventEditor`, `dragNote`, `virtualKey`) live here for the same reason
   `dragClipEdge` and `describeTrackMeter` do: testkit may include neither
   app/timeline nor app/eventui (testkit CONTRACT inv. 5). `grabEventEditor`
   temporarily DETACHES the dock's widget before sizing it — the dock's layout
   owns the geometry, so a `resize()` while parented is undone before `grab()`
   renders, and the first version of that grab produced whatever strip the
   hidden main window happened to allot.

10. **`beginRun(pos)` is issued at EVERY run start and nowhere else**
   (proposal 37 P3c, design D4 / 4.4; `docs/contracts/FREEZE_PROTOCOL.md` has
   the full rule). A run is an offline render or a play start. The shell owns
   the barrier because it is the only module that sees both the track tree and
   every transport entry point. Three play-start paths call it immediately
   before `twSpeaker::startOutput()` — `SMainWindow::startPlaying()` (the GUI
   Play button and the `toggle-playback` verb reach the speaker through
   different ones of these, so a barrier on only one would make determinism
   depend on which was used), `SApplication::setPlaybackRunning()` and the
   monitoring playback in `startRecording()` — and `startRender()` calls it
   before the render session's thread exists. Adding a FOURTH way to start the
   transport means adding a fourth call.
   It is NOT called from `setGlobalLocatorPos()`: a locate while stopped
   demands nothing (`requestSeek` is a no-op unless playing), and a locate
   while PLAYING deliberately keeps today's page-boundary splice rather than
   re-staling pages the RT thread is mid-way through serving.

11. **The automation recorder is ONE per app and lives here** (proposal 37 P6,
   design D5). Touch/Latch/Write are UI recorders, not engine modes, and BOTH
   the arranger's track fader (app/timeline) and the plugin parameter editor's
   slider (app/pluginui) feed the same pass — those two modules cannot see each
   other, so the shell is the only place the recorder can live. It is bounded
   by the TRANSPORT: `setPlaying(true)` opens the run (Write's overwrite window
   starts where the run did) and `setPlaying(false)` COMMITS whatever pass is
   open, as ONE `set-automation-points`, on the main thread. Everything in it
   runs on the main thread and nothing in it blocks; an action per tick — the
   obvious implementation — would put thirty entries a second on the undo
   stack.


How to test: full qxa suite (headless boots the shell); startup-layout
repro harness in STATE.md 2026-07-11. The barrier specifically:
`qxa.instrument_render_determinism`, `qxa.instrument_render_determinism_xproc`
(a CMake driver running two processes through one output directory — the only
way to make the fresh-process byte compare the render gate rests on) and
`qxa.instrument_locate_continuity`.

Known debt: there is no UI for the per-track MIDI port — the verb and the serialized attributes exist, the arranger does not offer them yet (P4's track inspector work). Nor is there a UI for the `midi/portId/<name>` mapping: the Options page LISTS the machine's ports but a name that does not match one has to be mapped by editing `smaragd.ini`.

Known debt: SApplication is the app-wide service locator — the SCC hub;
Phase 6 splits it into narrow context interfaces.

## The live lane (proposal 21 L1b, design D3/D4/D5/D9)

12. **`SLiveMonitor` is a sibling of `SMidiOutPump` and `SAutomationRecorder`,
    and it OWNS THE ARM/DISARM ORDERING.** Constructed with the app, destroyed
    FIRST in `~SApplication` (it joins a `std::thread` — the pump — and closes
    the input device, and both must happen on the main thread while the log
    sink is alive and before the speaker goes). A verb never does this work: it
    raises `SAppContext::liveLanesChanged()` and the sequence happens here,
    once, with the pump and the speaker in reach.

    ARM: `retireComponentNodes(closure)` → `setLiveOwned(true)` → wire the
    exclusion + invalidate PER MEMBER → read the ROOT REWIRE's
    `contentEpochNow()` as `flipEpoch` → open the INPUT → `openLive()` →
    publish the plan → `requestReposition()`.

13. **DISARM RELEASES OWNERSHIP BEFORE THE RE-WIRE, and the other order is a
    bug that looks like the right one.** `setLiveOwned(false)` runs while the
    exclusion is STILL applied — so no freeze can reach the chain between the
    two (`planPage` skips a nulled plug) — and only then are the flags cleared,
    the wiring re-applied and `flipEpochPrime` read. Releasing ownership
    afterwards lets the freeze path regain a still-live-owned chain, the next
    root page is frozen as SILENCE for those tracks, and the epoch gate flips
    the RT onto it: measured as a folder that went quiet for the whole 256 ms
    tail and eight `liveOwnedRefusals`. The departing members keep being
    rendered by the pump for `kDisarmTailMs` after that, carrying
    `flipEpochPrime`, which is what covers the hole while the root re-freezes.

14. **The tail only runs while the frozen lane is PLAYING.** With no root page
    being served there is nothing for the ring to cover, and holding a
    processor the freeze path is about to want would count refusals for no
    benefit. `refresh()` therefore finishes the disarm synchronously when the
    transport is stopped — and it must do so AFTER `current_` has been updated,
    because `finishDisarm()` computes what is really gone as "departing minus
    current" and closes the device when `current_` is empty.

15. **A transport edge rebuilds BEFORE `twSpeaker::startOutput()`, not after.**
    `setPlaybackRunning()` starts the readahead first and flips `isPlaying_`
    last, so a rebuild driven by the flag alone leaves a track that monitor
    Auto is about to release still live-owned while the readahead is already
    freezing its chain. `SLiveMonitor::transportAboutToChange(playing)` is
    called at the top of `setPlaybackRunning`; `transportChanged()` follows
    from `setPlaying` and adds the one explicit reposition.

16. **`SObject::invalidateRenderPath()` ON THE MIXER STALES THE MIXER AND THE
    ROOT REWIRE AND NOTHING BELOW THEM.** It walks from the project root and
    stales every chain CONTAINING the object it was called on. The exclusion
    therefore invalidates PER CLOSURE MEMBER; one call on the mixer leaves the
    members' own pages being served and is how a track stays silent after a
    hand-back.

17. **A render suspends every live lane and comes back as a FRESH ARM.**
    `startRender()` calls `suspendForRender()` BEFORE `beginRun()`, so the run
    barrier never meets a live-owned track; the session's `onComplete` posts a
    QUEUED `resumeLiveAfterRender()` because that callback runs on the render
    thread. "Fresh" is the point: the closure is recomputed from the model as
    it then stands, never restored from a snapshot.

18a. **The `Closure` master mode is REFUSED, not approximated.**
    `twlive::checkMasterShape` is asked BEFORE anything is re-wired, and a
    master that is not a unity sum with an identity map turns monitoring OFF
    with one log line and a tooltip. The plan builder can express the other
    mode, but the RT half of it is not wired — `twSpeaker` adds the frozen
    root page whenever the frozen lane is PLAYING and nothing reads
    `twLivePlan::masterLinear` — so a Closure-shaped plan would be summed on
    top of a root page that already contains those tracks and the arrangement
    would be heard DOUBLED. Unreachable today (`SStdMixer` builds exactly the
    linear shape); whoever adds a master insert chain lands here first, and the
    fix belongs in `twSpeaker`.

18. **The app never touches the ring and never renders on the pump.** Plans are
    built on the main thread and published with one `setPlan()`; the pump is
    the only thread that reads them. The re-rooted horizon demands (one handle
    per frozen input root, superseded by replacing the handle) are issued from
    a main-thread 40 ms timer here, because the pump may not demand.

## Live instruments (proposal 21 L2, design D2/D4/D8/D9)

19. **A MIDI-armed track contributes its CONSUMER to the closure, not itself.**
    `sliveplan::midiConsumerFor` walks the routing UP the way
    `STrack::eventFeed()` walks it down: a track that holds an instrument
    consumes the events, otherwise they go to the parent iff the track bubbles
    them up. So an armed CHILD of a folder drum machine is a MIDI SOURCE while
    the FOLDER is the live instrument and the thing that leaves the frozen sum —
    the child stays in it, because its own clips must keep playing (design
    section 3 case (iii)). A MIDI track whose notes would reach NO instrument
    is deliberately not a source at all: excluding it would trade the
    arrangement for silence.

20. **`setLiveEventSource` is the SECOND source, never a `setEventSource`
    swap, and never a member of `eventFeed()`** (design D2). The feed is
    re-applied by `STrack::syncInstrumentSlot()` from adopt / insert / remove
    and would silently overwrite a live source; `setEventSource` also clears
    continuity and bumps the param epoch, which an arm must not do per call;
    and the feed is ALSO read by `SMidiOutPump` and `assert-midi-events`, while
    a ring-draining `collect` has exactly ONE legal reader. The arrangement's
    feed is untouched by an arm.

    ARM order: `retireComponentNodes` → `setLiveOwned(true)` →
    `attachLiveEvents` → wire the exclusion → `flipEpoch` → publish →
    `requestReposition()` + `requestLiveChase()`. Installing the source BEFORE
    ownership would let a freeze worker collect the ring.

    DISARM order (the mirror, and inv. 13's rule holds unchanged):
    `detachLiveEvents` (all-notes-off flush → `setLiveEventSource(nullptr)` →
    thru off + `panic()`) → `setLiveOwned(false)` (which forgets continuity) →
    un-wire → `flipEpochPrime` → the tail plan. The flush is best-effort within
    the blocks the pump still renders; what GUARANTEES no hanging voice is the
    hand-back itself, because the freeze path's first render resets every
    instance.

21. **A live source is keyed by its CONSUMER and is never rebuilt on a
    republish.** Two armed children bubbling into one folder instrument share
    ONE source and therefore one ring, which is also the only shape an SPSC
    ring allows. It holds the ring cursor and the HELD-NOTE TABLE, so
    rebuilding it under a finger would drop the note being played;
    `attachLiveEvents` therefore prunes and adds, and touches nothing that is
    already correct.

22. **`SMidiInputHub` owns every open input port and never closes one until
    teardown.** Opening a MIDI device is not free and a disarm/arm cycle must
    not drop it — but the load-bearing reason is that
    `CaptureMidiInput::inject()` is a NO-OP on a closed port, so closing one on
    disarm would silently swallow a script's events between two phases of a
    case. Its enumeration probe is constructed FIRST, before any listening
    port, so a listening port is always the newer `CaptureMidiInput::active()`
    and a headless injection reaches the port the live lane drains. The
    computer keyboard is opened EAGERLY at construction: it is in-process, and
    the piano-roll dock has to be able to play it before any track is armed.
    `recorderSink()` is L4's hook and is spelled out now so the shape is fixed
    before there is a second consumer to argue with.

23. **MIDI-thru shares the sequenced feed's scheduler, and disarm PANICS it.**
    Two schedulers on one port would be two threads racing one device, and
    thru and playback have to interleave on the wire in the order the events
    happened; they do not collide because they use different RINGS (inv. 21 of
    tw/devices). The thru port is the ARMED track's `midiOutPort`, falling back
    to the consumer's — the folder-drum-machine shape, where the child has no
    port of its own. A key held when the user disarms is otherwise a stuck note
    on their hardware synth, which is the one failure mode a performer never
    forgives.

24. **A LIVE LANE DROPS BLOCKS UNDER LOAD, AND 1024 FRAMES IS THE BOUND.** The
    RT sums a ring entry only when its stamp matches the frame it is delivering
    (design D2); a miss is SILENCE plus `twLiveMixRing::misses`, and that
    silence is ONE DEVICE BLOCK wide by construction. Measured at
    `SMARAGD_REVAL_WORKERS=8` - eight revalidation workers plus the readahead
    against a pump that must wake every ~21 ms - the live lane misses a block
    in roughly 2 runs in 25. A case that asserts a sub-block gap on a
    PUMP-rendered window is therefore asserting something the design does not
    offer: `live_instrument_disarm_playback` was 46/50 until its 512-frame
    bound moved to the FROZEN window, where it is deterministic and reads
    exactly 0.040405 with a gap of 1 frame on every run. 1024 is the same bound
    L1b's own live cases already carry.
