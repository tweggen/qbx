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
   thread; publishPosition() audio thread, atomics. `locatorHeldElsewhere()`
   is RETIRED (proposal 21 L3b): a recording is an ordinary transport run
   and the OUTPUT publication is the playhead authority in every mode.
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


## Audio recording (proposal 21 L3b, design D6/D7/D9)

`saudiorecorder.{h,cpp}` + `srecordplacement.h`. `SAudioRecorder` is a sibling
of `SMidiOutPump`, `SAutomationRecorder` and `SLiveMonitor`: one per
`SApplication`, main thread only, owning a 100 ms `QTimer`. It is what a record
start and a record stop MEAN in the app.

19. **THE PLACEMENT CONVERSION IS ONE NAMED FUNCTION**, `srecordplacement.h`,
    and it is coded exactly once:

        placementFrame(k) = P0 + k - inputLatencyProj - outputLatencyProj
                                   + userOffsetProj

    `P0` is the project frame that capture frame 0's HOST TIME maps to through
    the ENGINE-owned clock anchor (`twEngineClock::read()`'s
    `{deliveredFrame, hostNs}`) — never `SApplication`'s locator, which is a
    UI-thread value and would re-derive a publish-lag correction the clock
    already carries. The sign is design D6's derivation and is not re-argued:
    the performer plays to what they HEAR (`outputLatency` earlier) and the
    microphone's sample reaches the ADC `inputLatency` before delivery.

20. **`recordingOffsetMs` is POSITIVE = EARLIER**, the app-wide convention (37
    P7's `midi/offsetMs`). The design writes the term with a PLUS, so the
    negation lives in ONE setter — `SRecordPlacement::setUserOffsetMs` — with
    the reasoning beside it, rather than being spread over call sites.

21. **THE ANCHOR IS TAKEN ONCE, RETROSPECTIVELY, AND ONLY FROM THIS RUN.**
    Capture frame 0's host time precedes the first publication when a take
    starts from a STOPPED transport (the readahead primes before the RT
    publishes), so the bridge stamps `captureStartHostNs()` and the mapping is
    applied BACKWARD as soon as an anchor appears. The publication counter is
    process-global and monotone, so the anchor must have `seq >` the one
    sampled at start — the same trap `SMidiOutPump::resetRun` records.

22. **THE OUTPUT LATENCY IS READ WITH THE ANCHOR, NOT AT START.** A take begun
    from a stopped transport OPENS the device as part of starting, so at
    `start()` there is no backend to ask and the term would silently be 0.
    By the time the clock has published, the device is open by construction.
    The scaling is `meterLatencyFrames()`'s (proposal 34), restated rather than
    called because that accessor answers 0 unless `isPlaying_`.

23. **THE TRIM FLOOR IS THE TRANSPORT START, NOT THE RECORD START.** Design D6
    trims "frames captured before the transport start". That is the record
    start only when the take STARTED the transport; recording into a run that
    was already playing legitimately places audio EARLIER than the button
    press — that is what latency compensation IS — and trimming to the button
    press would throw the compensation away again. A Cubase-style catch range
    is NOT implemented.

24. **THE TRANSPORT EDGE IS LAST IN `start()`, AND AFTER `active_`.** Monitor
    Auto is "input while stopped OR RECORDING" (design D9), so the plan has to
    be built with `isRecordingActive()` already true or plain Play would take
    the input away from the take. And it goes through `setPlaybackRunning()`
    like any other transport start — the old path set `isPlaying_` directly and
    told neither the MIDI-out pump, nor the automation recorder, nor the live
    monitor.

25. **THE APP HAS ONE INPUT PUMP AND `SLiveMonitor` OWNS IT.** The bridge is
    the object that drains the input device's ring (design D7); monitoring pops
    its live ring through `SLiveAudioInputSource::pull` -> `pullLive`, and
    recording opens a capture SEGMENT on the same bridge with `beginCapture()`.
    `SAudioRecorder` BORROWS it — `acquireBridge`/`releaseBridge` is a hold
    count, and it is what stops `closeInputIfUnused()` pulling the device out
    from under a take when the last monitored lane disarms mid-recording. A
    record start while monitoring therefore does NOT gap the monitored signal.

26. **THE GROWING CLIPS ARE NOT ACTIONS; THE PLACEMENT IS ONE.** The clip that
    appears at record start is a direct model mutation, like auto-disarm: it is
    transient UI state that the one undoable step at stop replaces. At stop the
    growing clips are removed FIRST (else `place-recording` would see them as
    covering columns and stack a take on them) and then one macro of
    `place-recording` calls — one per armed track, one per LOOP PASS — is
    submitted, so a whole take is ONE undo.

27. **LOOP PASSES ARE ARITHMETIC, NOT WRAP DETECTION.** The conversion is
    linear in capture frame, so the pass a frame belongs to is
    `floor((placement - loopIn) / loopLen)`; the recorder splits at those
    boundaries and emits one `place-recording` per segment, all at the loop
    start. `place-recording`'s own proposal-17 planner then turns pass 2 onto
    pass 1's column as a TAKE. A poll-based wrap detector could not see a wrap
    that happened between two 100 ms ticks.

28. **PUNCH IS A CLAMP, NOT A RACE.** The take ends once the placement passes
    the out point, and the placed span is then clipped to `[in, out)` however
    far past it the tick actually got — so the placed clip is exact to the
    frame. The project has ONE range: it is the LOOP when Cycle is on and the
    PUNCH region when it is off.

29. **THE RECORDING DIALOG IS NON-MODAL AND POLLS.** It used to `exec()` inside
    the record-button handler, blocking the whole app for the length of a take.
    It now shows, polls `SAudioRecorder` at 10 Hz, follows a stop that happened
    anywhere else (a punch-out, the record button again), and stops the take
    when closed.
