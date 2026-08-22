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
   (Main's interim `recordMonitorPrimingFrames` / "held while nothing is
   audible yet" mechanism from PR #53 is superseded by the same rule: the
   take is anchored on the engine clock and pre-roll is trimmed — see
   `SRecordPlacement`.)
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
   `restoreWindowLayout()` cannot restore them (inv. 4). There are now SEVEN:
   `dock_extern_file_list`, `dock_track_detail`, `dock_log`,
   `dock_clip_properties`, and — since proposal 37 P4 —
   `dock_event_editor` (bottom, tabified with the Log) and
   `dock_virtual_keyboard` (bottom, tabified with the editor), and — since
   proposal 38 gate 2, moved to RIGHT and tabified with Clip Properties by
   AC-b1's sibling AC-e1 (2026-08-21) — `dock_media_browser`. The View menu
   carries every dock's toggle (Ctrl+Shift+E for the editor; the media browser
   has no shortcut). Constructing the media browser CONTACTS NOTHING — a media
   source is registered, which does no I/O, and opened only when it is
   SELECTED — so a dock restored with `media/lastSourceId` naming a dead
   server costs a banner at the next click rather than a hang at launch.

   **Every dock but the media browser starts hidden.** The media browser
   starts VISIBLE by default (AC-e1) — but that is a PRE-RESTORE default only,
   never an override of `restoreState()`: on any run where `ui/windowState`
   is non-empty, `restoreWindowLayout()` (called after every dock exists, per
   this same invariant) restores whatever the user last left it as, exactly
   as it already does for the other six. The default is suppressed under
   `SApplication::isTestCaseMode()`, for the same reason `restoreOpenEditors()`
   (proposal 33 D2) is: a headless qxa run must not start showing a window it
   did not show before. **Dock LAYOUT stays PER-USER, never per-project** — a
   `ui/windowState` key in the shared `smaragd.ini`, not a `.qxp` property —
   and that is unchanged by AC-e1; only the constructor's pre-restore default
   moved.
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

20a. **THE OFFSET IS READ UNDER THE DEVICE `start()` RESOLVED, AND THAT
    RESOLUTION IS NOT THE SETTINGS DEFAULT.** `deviceName_` is the armed
    track's own `trackInput` device first, then whatever the monitor already
    has open, then `audio/inputDeviceId`, then `"default"` — the monitor's own
    order, so a record start never fights it for the device (commit 3c5fd6f).
    `audio/recordingOffsetMs/<deviceName_>` is then the key, and it is the key
    `SOptionsDialog::applyAudioPage()` writes under, so the production round
    trip agrees. Two consequences that have both been paid for:
    **a miss is silent** — `SSettings::recordingOffsetMs()` answers 0.0 for a
    device it has never heard of, which is exactly what an uncalibrated device
    answers, so nothing downstream can tell the two apart and the resolved name
    is LOGGED at every start for that reason; and **a headless case must NAME
    its device** (`set-track-input`), because the third rung is a per-user
    preference the suite does not set — `record_offset_zero` relied on it and
    silently stopped gating anything the day the machine's owner picked an
    input device in Options. Do NOT "fix" a miss by falling back to
    `"default"`: the number is a physical correction for ONE device's latency
    error, so lending it to another device is worse than the zero it replaces.

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

### Known limitation: a record stop STOPS the transport

`stop()` ends the transport and returns the playhead to the record start,
whatever the take was recorded into. That is right when the take STARTED the
transport, and it is what the previous (modal) implementation did; it is not
what a punch drop-out should do to a run that was already playing, where a
reference DAW keeps rolling and leaves the playhead alone. Changing it is a
transport-behaviour decision beyond L3b's brief and it interacts with
`toggle-playback`'s handling of a redundant Play, so it is recorded here rather
than guessed at. The trim floor already distinguishes the two cases
(`wasPlaying_`), so the information a fix needs is present.

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

## MIDI recording (proposal 21 L4 = 37 P8b, design D6/D8/D9)

25. **`SPlayheadClock` is THE host-time <-> project-frame conversion, and there
    is exactly one of it.** `SMidiOutPump` asks it forward ("what host time is
    frame F heard at?") to schedule a message; `SMidiRecorder` asks it backward
    ("what frame was being heard when this byte arrived?") to place a recorded
    note. It is the pump's own anchor discipline moved out unchanged:
    re-anchored on every position PUBLICATION rather than every position CHANGE
    (the two differ exactly once, at the start, and that is the time that
    matters - measured: anchoring on a change put the first note of a run 59 ms
    early), the publish-lag correction (`twSpeaker` publishes AFTER the pull, so
    the frame just delivered is `P - bufferFrames`), the device-latency term
    through `meterLatencyFrames()`, and the GUARD on the first anchor of a run
    (a locate is published by the UI thread before the engine's seek lands, and
    anchoring on that publication would put a whole window in the past). A
    second implementation of any of that would be a second set of corrections to
    keep in step.

26. **`SMidiRecorder` maps NOTHING on its tick.** The 20 ms poll pops each
    port's recorder ring into a buffer of `{hostTimeNs, bytes}` and offers the
    playhead to the clock; the model is touched only at the stop, inside one
    undo macro. That is what makes the mapping RETROSPECTIVE by construction
    rather than by a special case: a take begun from a stopped transport
    captures its first messages before the RT has published anything, and
    backward extrapolation on a clock linear in host time is exact. The
    conversion, in one line:

        projectFrame(msg) = clock.frameAtHostNs(msg.hostTimeNs) - inputOffsetProj

    `frameAtHostNs` already answers "the frame being HEARD", so there is no
    separate output-latency term here - design D6's derivation, that the
    performer plays to what they hear. `inputOffsetProj` is the port's
    `midi/inputOffsetMs` and its sign is the app-wide one: POSITIVE = EARLIER.

27. **The split between the two recorders is by TRACK INPUT, never by two
    record buttons.** `SApplication::startRecording()` runs both: an armed track
    whose `trackInput` is `midi:`/`keyboard` belongs to `SMidiRecorder`, every
    other armed track to `SAudioRecorder` (`collectArmed` in each filters on
    `hasMidiTrackInput()`, in opposite directions). Without that filter a
    MIDI-armed track would be given an audio WAV sink and a growing audio clip
    out of an input device it never asked for.

    ORDER, and it is load-bearing: the MIDI recorder starts FIRST and does not
    touch the transport; the audio recorder starts second and owns the transport
    edge whenever it has a take of its own; only a MIDI-ONLY run starts the
    transport from `startRecording` itself. Monitor AUTO is "input while stopped
    OR RECORDING" (design D9), so `isRecordingActive()` has to be true before
    the live plan is rebuilt by that edge - which is why the MIDI half sets its
    `active_` before anything transport-shaped happens.

    At the stop the MIDI recorder commits FIRST, while the transport is still
    running: its anchor is only valid while the RT thread is publishing.

28. **The recorder's ring is a SECOND consumer of the fan-out, and the live
    lane's is untouched.** `SMidiInputHub::recorderSink(port)` mints one sink
    per PORT and keeps it for the process (design D8: the device thread writes
    one ring per consumer, so SPSC stays SPSC). Two armed tracks on one port
    SHARE that sink, because a ring has exactly one consumer; the per-track
    channel filter is applied when the buffer is read, not when it is filled.
    At a record start the ring is DRAINED, never `clear()`ed - `clear()` is only
    safe while the producer is known to be idle, and a performer's finger is not.
    A retrospective `place-retro-midi` (design D8) would keep what was drained;
    it is not implemented.

29. **Loop passes are ARITHMETIC on wrap-counted frames**, exactly as they are
    for audio: `floor((f - loopIn) / loopLen)`, never wrap detection, because a
    20 ms poll cannot see a wrap between two ticks. The tick folds
    `iteration * cycleLength` into the clock's anchor so every frame the
    recorder computes is unwrapped and monotone. **Every pass is PLACED AT THE
    LOOP START** - `passStart(pass)` is unbounded (pass 2 of a 2 s cycle starts
    at 192000) and placing there would put pass 2 three loops to the right
    instead of stacking a take on pass 1's column.

30. **A note still held at the stop is CLOSED at the stop frame, and a note
    whose mapping lands before its pass is CLAMPED into it, never dropped.** A
    recording with an unterminated note is not a recording; and being early is
    the NORMAL case for the first messages of a take begun from a stopped
    transport, exactly as being late is the normal case for a live event
    (`twLiveEventSource`). Both are counted (`clampedNotes()`), not silent.

31. **ALL-NOTES-OFF ON STOP IS NOT SENT FROM THE RECORDER.** Closing the held
    notes in the RECORDING is its half. The sounding half already has two
    owners: `SMidiOutPump::stop()` panics every MIDI-out port its run used, and
    L2's `detachLiveEvents` flushes the live source's held-note table at disarm.
    A third flush would be a duplicate all-notes-off on the user's hardware, and
    the recorder is not the thing holding those notes.

32. **THE METRONOME IS A LIVE LANE, AND A LANE EXISTS IFF `armed u monitor u
    METRONOME`** (proposal 21 L5, design D1/D9). The click joins the plan as a
    SYNTHETIC track at the output — no `STrack`, no processors, unity gain,
    identity map — carried on `SLiveClosure::metronome`, a FLAG rather than a
    member. That is what leaves the entire arm/disarm protocol untouched:
    nothing is retired, nothing is live-owned, no plug is nulled, so a
    metronome-only lane cannot change one byte of what the frozen graph
    produces.

    Three consequences that are easy to get wrong and were:

    - **A metronome-only lane leaves through NO DISARM PATH.** `leaving` is
      empty because it owned no track, so `finishDisarm()` never runs and the
      pump would keep clicking off the old plan forever. `refresh()` stops the
      pump, drops the source and closes the lane in the `want.empty()` branch.
      Before L5 an empty live set could only be reached by a track LEAVING, so
      the path did not exist.
    - **No exclusion means no epoch gate.** `flipEpoch` exists so the RT does
      not sum a ring entry onto a root page that still CONTAINS the armed track.
      A lane with no track members bumped nothing, so `publishPlan` passes 0 —
      otherwise the click would be gated off until an unrelated re-freeze
      happened to land.
    - **It must not open the microphone.** `SLiveClosure::needsInput()` (the
      SOURCES, not `empty()`) is what `closeInputIfUnused` and `acquireBridge`
      ask, or a click-only lane would hold the input device and push into a live
      ring nobody pops.

33. **COUNT-IN AND PRE-ROLL ARE TRANSPORT BEHAVIOURS AROUND THE RECORDERS**, and
    they live in `SApplication` because neither recorder owns the transport on
    its own (proposal 21 L5). THE READING TAKEN, stated once here:

    - **Count-in**: the click plays for N bars BEFORE the record position while
      the transport is STOPPED. THE PLAYHEAD DOES NOT MOVE. Recording then
      begins AT THE LOCATOR, so the placed clip lands exactly where it would
      have without a count-in and the capture holds N bars of clicks before it.
      That is Cubase / Logic / REAPER. The rejected reading — roll the count-in
      bars ON the timeline, so the take lands N bars later — makes a preference
      silently move the user's recording.
    - **Pre-roll**: the transport STARTS N bars before the locator and rolls
      through them, so the arrangement is heard running up to the entry, and
      recording begins when the playhead reaches the locator. The take is
      recorded into a run that was already playing, so `SAudioRecorder` sees
      `wasPlaying_` and nothing is trimmed — which is what latency compensation
      IS, and is why a pre-rolled take lands a few thousand frames BEFORE the
      locator while a counted-in one lands exactly ON it.
    - They compose: the count-in counts, then the pre-roll rolls.

    Neither is offered while the transport is ALREADY running: punching in while
    the tape rolls has no count-in in any DAW, and a pre-roll would mean seeking
    backwards under a running take.

34. **THE COUNT-IN ENDS ON DELIVERED FRAMES, NOT ON A TIMER**, and the click
    grid is anchored AT the record position (proposal 21 L5). Two things the
    obvious implementations get wrong:

    - `twLiveMixRing::framesDelivered()` is the clock. While stopped there is no
      engine clock at all, and a `QTimer` of the count-in's DURATION would
      measure the Windows scheduler against a grid the gate asserts to 38
      frames. A wall-clock WATCHDOG still exists, at twice the preamble plus two
      seconds, because a device that never opens delivers no frames and a
      transport that never starts is a hang.
    - The grid counts FORWARD from the locator. Running the pump's virtual
      counter BACKWARDS from `locator - N bars` was the first design and is
      wrong: at a locator inside the first N bars it produces NEGATIVE
      positions, and `twlive::gateEpoch` discards a ring entry stamped below
      zero as an unwritten slot — so a count-in at bar 1, the commonest case
      there is, would have been silent.

35. **THE CLICK STOPS BEFORE THE TRANSPORT STARTS; THE LANE STOPS AFTER**
    (`SLiveMonitor::muteCountIn`, proposal 21 L5). Both orders are load-bearing
    and both were paid for by a failing gate:

    - the click has to stop FIRST because the count-in grid is in the
      ARRANGEMENT's position domain, and the transport start repositions the
      pump back to the locator — which re-renders the count-in's first beat.
      Measured as a fifth, accented click after a one-bar count-in;
    - the lane has to survive because dropping the last live lane calls
      `twSpeaker::closeLive()`, which CLOSES the device while the frozen lane is
      still stopped; the transport start would then re-open it, and the capture
      backend clears its recording at device start — taking the whole count-in
      with it.

    So `muteCountIn()` closes the click's range to zero length and keeps the
    source in the plan; `endCountIn()` drops the lane once the take is running,
    by which time the frozen lane is holding the device.

36. **THE LATENCY READOUT DESCRIBES THE DEVICES, NOT THE PLAYHEAD** (proposal 21
    L5, design D5). `SApplication::outputLatencyFramesProject()` is
    `meterLatencyFrames()` WITHOUT its "only while playing" gate, and
    `meterLatencyFrames()` is now one line on top of it. The gate belongs to the
    COMPENSATION — shifting a position nobody is playing is meaningless — and
    not to the READOUT, which has to show a number the moment a device opens,
    including when arming opens it with the transport stopped. `latencyReport()`
    reads the input side off the open `CaptureBridge`, whose reported latency is
    already in PROJECT frames (it delivers at the target rate), so only the
    output term is rate-scaled.

    **PLUGIN DELAY COMPENSATION IS OUT OF SCOPE** (proposal 37 P9). Every mount
    that shows a plugin's reported latency says so, because the live lane has no
    delay line anywhere: what you hear through it really is late by that much.

37. **"CLICK WHILE RECORDING" (`SOpt::ClickWhileRecording`) IS A SECOND GATE ON
    THE CLICK, NOT A REPLACEMENT FOR THE METRONOME SWITCH** (proposal 21 L5
    follow-up, the metronome button's right-click menu). `sliveplan::
    metronomeWanted` reads it fresh on every `SLiveMonitor::refresh()`:

    ```
    if( countIn )      return true;
    if( !metronomeOn ) return false;
    if( recording )    return clickWhileRecording;
    return playing;
    ```

    `recording` is checked, not `playing || recording` as before this option
    existed — recording implies the transport is running, so an `||` would
    make the checkbox unable to silence a take. Plain playback (not recording)
    is unaffected by the flag either way, and a count-in still overrides
    everything (invariant 33 above): it is not "recording" yet — the playhead
    has not moved.

    Being a per-USER `SSettings` key, not a project property, it raises no
    `propertyChanged` the way `SProjectProps::Metronome` does (`SApplication`'s
    ctor wiring) — so the menu's checkbox calls `SApplication::
    liveLanesChanged()` itself after writing it, the same call the metronome
    switch's own signal handler makes, or a live lane already up would not see
    the change until its next unrelated rebuild.

    "Count in while recording", the menu's other item, is deliberately NOT a
    second SOpt key: it is a convenience on/off toggle over the SAME
    `SOpt::CountInBars` the Options dialog's spinbox edits, because 0 already
    means "off" everywhere that key is read (this invariant's `countIn` term
    included). `SMainWindow` remembers the last non-zero bar count in-memory
    (`metronomeLastCountInBars_`) so unchecking then rechecking restores it;
    falling back to 2 bars if nothing was stashed this session is the "or
    default" the feature was specified with, not an oversight — it is NOT
    persisted across a restart.

    Gate: `metronome_click_while_recording.qxa` (a take with the flag at its
    default reads clicks; a second take with it off reads none, while the
    project's own metronome switch stays ON throughout — the whole point is
    that the second take is silent because of THIS option). **NOT gated:** the
    context menu itself — there is no testkit verb for a context menu anywhere
    in this repo.

## The media layer's two hooks (proposal 38 gate 3)

Three injections, split across two classes because the status bar belongs to
one of them and the composition root to the other.
`SApplication::initMediaLayer()`, called from the ctor beside
`initPluginRegistry()`, does the first two — and in both cases the reason is
that `app/media` is **app_core**:

1. **The cache's ROOT and CAP.** `<configDir>/mediacache` and
   `SOpt::MediaCacheCapMB` both live behind `SSettings`, which app_core cannot
   see. Exactly the shape `initPluginRegistry()` already uses for the plugin
   scan cache's path. `SMARAGD_MEDIA_CACHE_DIR` beats the injection, because the
   knob exists so `ctest -j4` can isolate four processes.
2. **THE PLACEMENT ITSELF.** `SAddSampleAction` is app_objects, one LAYER above
   app/media, so nothing there can construct one; no edit to
   `tools/check_layering.py` would change that. `smediadrop::setPlacementHook`
   takes a lambda that re-resolves the target track's index-path FROM THE
   TRACK'S OWN IDENTITY at completion — never from a path captured at drop time
   (trap T18) — and submits the same `add-sample` the `file:` branch does. An
   empty path means the track is no longer in the tree, and the only safe answer
   to that is to place nothing.

**THE DESIGN'S OWN SKETCH OF THIS CALL COULD NEVER HAVE COMPILED**, and it is
worth knowing why rather than rediscovering it. Proposal 38 §B.5 printed the
arranger's new branch as `smediadrop::placeWhenLocal( ref, trackId, timePos,
this )` — a `QWidget*` handed into a module whose contract forbids widgets, to
a function that would then construct an `SAddSampleAction`: a class declared in
app_objects, one LAYER ABOVE the app_core module the function lives in, so the
include does not even resolve. Neither half is a `check_layering.py` entry
anybody could have added. The hooks are what the shape actually is; the
proposal has been corrected.

`SMainWindow`'s ctor installs the third piece, `setStatusHook`, because that is
where the status bar is: `app/media` owns no widget by contract, so "fetching
…", "could not fetch …", "the target track is gone" and the unsaved-project
warning reach the user through a hook rather than through a `QWidget*`.

**A pending placement is cancelled from `setCurrentProject`**, not from a close
handler: proposal 38 §B.5 says closed OR SWITCHED, and a close is one case of a
change rather than the only one.

## Nextcloud accounts (proposal 38 GATE 5b)

37. **`SMediaAccountManager` is the accounts model, and it owns three things a
    dialog is not allowed to own separately.** Persistence of url/user
    (`SSettings`), the password (`SSecretStore`, §B.8), and the LIVE
    `SWebDavMediaSource` instances registered with `SMediaRegistry` all go
    through this ONE object (owned by `SApplication`, never null after
    construction), so an account added through Options -> Media is
    immediately live for the media browser's source combo without the dialog
    and the dock having to agree on anything. It also implements
    `smedia::CredentialProvider` (`app/media/smediacredentials.h`) and
    installs itself as the process-wide provider at construction, clearing it
    at destruction — the seam `main/media` declares and `main/shell` is the
    only implementation of, exactly as `SAppContext` is.

38. **The Options dialog and every testkit verb call the SAME manager
    method.** `SOptionsDialog::onMediaSaveAccount()` and the testkit's
    `set-media-account` both call `SMediaAccountManager::setAccount()`
    directly — there is no second, UI-only validation path — so "an
    unparseable URL is refused at the dialog" (AC 13) is the SAME refusal a
    script can drive without synthesizing a button click, the same
    `set-option` bypasses a spin box to reach `SSettings` directly.

39. **A password is never re-read into the UI.** `loadMediaAccountIntoForm()`
    clears the password field and reports only `SMediaAccountManager::
    account()`'s `passwordStatus` (`Unset`/`Set`/`Undecryptable`) — the
    plaintext is fetched from the store only inside `setAccount()`'s own call
    to build the `Authorization` header for the source it registers, and
    nowhere else holds onto it past that call.

39a. **"Test connection" with an EMPTY password field tests the SAVED
    credential** — `SOptionsDialog::onMediaTestConnection()` calls
    `testAccountConnection()`, never the bare `testConnection()`. Inv. 39 is
    what makes this necessary rather than convenient: the field is empty
    immediately after a Save and again whenever an account is picked out of the
    list, so the bare call sends `Basic base64("user:")` and reports a **401
    for an account that browses perfectly well** — a status the user reads as
    the server's answer, which is exactly the wrong diagnosis. The fallback
    fires only while `url`/`user` still match the saved account (an edited URL
    has no saved password to test), uses the SAME header
    `authorizationHeaderFor()` gives the browser (so a green Test and a working
    browse cannot disagree), and announces itself as `(saved password)`. Every
    no-credential outcome is a SENTENCE naming what to do, never a manufactured
    HTTP status. A typed password always wins — it is the one thing on screen.
    Gated by `media_webdav_browse` item (7), with the wrong-credential negative
    control that stops it passing on a server that ignores the header.

40. **Remember OFF SCRUBS, it does not merely skip.** `setAccount()` calls
    `SSecretStore::remove()` whenever `remember` is false OR the resolved
    backend cannot persist — never only when there was nothing to remove —
    because a checkbox that only ever ADDS a secret and never takes one away
    is not really an "off" state (AC 9, §B.8 rule 4).

41. **`SMediaAccountManager`'s own `QSettings` is a SEPARATE instance from
    `SSettings::instance()`'s, pointed at the SAME on-disk file** (identical
    `IniFormat`/`UserScope`/`"Smaragd"`/`"smaragd"` quadruple — Qt shares one
    `QConfFile` per path within a process, so the two coexist safely and a
    write through either is visible through both). This is deliberate, not
    an oversight: `SSecretStore`'s header explains why it never reaches for
    `SSettings` itself (its OWN unit test must never touch the real INI), so
    the composition root that owns a REAL, persistent `SSecretStore` has to
    hand it a `QSettings` explicitly, and that `QSettings` cannot be
    `SSettings`'s private member (there is no accessor for it, on purpose).

## The secret store (proposal 38 GATE 5a)

`SSecretStore` (`app/shell/ssecretstore.h`) is where a password lives. It sits
beside `SSettings` rather than inside it for one reason that governs everything
below: **`SSettings` is the INI, and a secret must never be in the INI in a
form anyone can read.**

42. **A SECRET NEVER GOES INTO `smaragd.ini` IN PLAIN TEXT, AND THERE IS NO
    WEAK TIER.** Five backends, chosen at build time by platform and
    overridable at run time: `dpapi` (Windows, `CryptProtectData` with
    `CRYPTPROTECT_UI_FORBIDDEN`, user-scoped, ciphertext base64 in the INI),
    `keychain` (macOS login keychain, `kSecClassGenericPassword`, service
    `com.smaragd.media` — not in the INI at all), `libsecret` (Linux Secret
    Service, `TW_HAVE_LIBSECRET`, optional), `none` (nowhere: "Remember" is
    DISABLED and the password is session-only), and `memory` (a
    process-lifetime map, the test backend). There is deliberately no
    encrypt-it-ourselves fallback — this tree links no cipher, and a scheme
    that would have to document itself as protecting nothing is a checkbox
    rather than a control (§B.8). A real backend that fails at RUNTIME (a
    locked keychain, a Secret Service that is not running) degrades to `none`'s
    behaviour with a reason, never silently to something weaker.

43. **THE SCHEME TAG IS LOAD-BEARING: a blob is decrypted only through the
    backend NAMED AT THE KEY, never the instance's currently-selected one.** A
    DPAPI blob is user- and machine-bound by design, so an INI copied to
    another machine, another account, or a roamed `%APPDATA%` **cannot**
    decrypt — and must fail READABLY. It reports `Result::Undecryptable`, the
    accounts page says "re-enter the password for this account", and **nothing
    is sent to the server**. Without the tag the same situation is a garbage
    credential in an `Authorization` header and a 401 nobody can explain.

44. **A SECRET NEVER APPEARS IN A LOG, A `describe()`, AN EXCEPTION OR A URL.**
    Every diagnostic names the KEY and the SCHEME, never the value;
    `describeMediaPage()` reports `password=set|unset|undecryptable` and never
    a length or a prefix. The gate asserts the absence of the password **AND
    its base64 spelling** — a Basic header leaks `base64(user:password)`, not
    the password, so a case grepping only for the literal would pass with the
    credential sitting in the log in plain sight. It must NOT assert `"Basic "`
    absent: that forbids the correctly redacted line
    (`Authorization: Basic <redacted>`) and every sentence containing the word,
    which would force redaction to erase the scheme name to satisfy its own
    gate. `media_secret_redaction` is the case.

45. **A SECRET NEVER ENTERS A `.qxp`.** An account is machine-local
    configuration, like a device id. Nothing in proposal 38 touches the project
    file, and this class only ever reaches the `QSettings` it was handed and
    the platform store.

46. **`SMARAGD_SECRET_BACKEND=dpapi|keychain|libsecret|none|memory` overrides
    ahead of the platform choice, and `memory` is the `--test-case` default.**
    It mirrors `SMARAGD_AUDIO_BACKEND` / `SMARAGD_MIDI_BACKEND` exactly, and it
    is read ONCE per process — which is why `media_options_no_store` needs its
    own CTest entry to see the `none` behaviour rather than switching mid
    script. Two reasons for the default, and the second is the sharper one: a
    headless suite must not write into the developer's real keychain or Secret
    Service, and on macOS a keychain access from a background test can BLOCK ON
    A UI PROMPT NOBODY CAN SEE — the `qoffscreen` failure mode again, a case
    burning its whole timeout at ~0 % CPU. `memory` is a REAL store with
    process lifetime, because the options-page cases need one that round-trips.

47. **INSTANTIATION NEVER REACHES FOR `SSettings::instance()`** — the caller
    passes its own `QSettings*`. That is what lets `secret_store_test` point
    the class at a private temp-file INI and run un-`RUN_SERIAL` beside the qxa
    suite without ever touching a real credential, and it is why inv. 41's
    second `QSettings` instance exists.

**What this does NOT buy, stated plainly because a credential store that
oversells itself is worse than one that does not.** Nothing here changes what
goes over the WIRE: HTTP Basic sends the password to the server on every
request, TLS is what protects that, and the app password is what limits the
blast radius — encryption at rest and encryption in transit are different
problems and only the first is solved. The plaintext is a `QString` in RAM
while an account is open, and Qt strings are implicitly shared, so there is no
honest way to zero them; the window is bounded by fetching at open and
releasing when the last source for the account closes, and no more than that is
claimed. And whether DPAPI, Keychain and libsecret actually resist an attacker
is **their** business — this proposal gates the plumbing and the failure modes,
not the cryptography.

## inv. 48-50 — missing samples and the self-contained banner (2026-08-22)

**inv. 48. `reportMissingSamples_()` is ONE dialog per LOAD, naming every file,
and it is the LAST thing `openProjectFile` does.** Last for the same reason
`SPluginNativeEditor::restoreOpenEditors` is: a modal box is stacked over a
finished window, never over a half-built one — and the self-contained banner is
already refreshed by `createDocksToolbars()` earlier in the same function, so
what the user reads BEHIND the dialog is correct.

It **returns immediately when `SAppContext::testOutputDir()` is set.** A modal
`exec()` in a headless run blocks until CTest kills the process at its timeout,
and the failure then reads as a HANG rather than as a dialog nobody can see —
the `qoffscreen` failure mode this repo has already paid ten minutes of gate
time for once. The same guard `SPlainWave::setWave` uses. The paths reach the
log either way (`SPlainWave` warns per file, and this logs the count).

The text states what was NOT done, deliberately: the previous behaviour was the
opposite (clips were dropped), and a user who met it once will assume it still
holds unless told otherwise.

**inv. 49. The resources dock holds a CONTAINER — banner on top, list below.**
The banner cannot live inside `SExternFileList`: that is a `QTreeWidget` in
`app_model`, a layer that may include no other app module
(`tools/check_layering.py`, `APP_DEPS['model'] == set()`), so it can reach
neither the copy helper the button needs nor the project's own
`externalMediaPaths()` semantics beyond the model call itself. Its visibility is
recomputed by `refreshSelfContainedBanner_()`, which is re-run on
`createDocksToolbars()`, on `SProject::externFileAdded`/`externFileRemoved`
(dropping a library sample makes a self-contained project stop being one at that
moment, not at the next open), and after every SAVE — a Save As moves the ANCHOR
every reference is measured against, so self-containment can change with no file
changing at all.

**inv. 50. `collectExternalMedia_()` confirms, delegates, and reports — it does
not copy.** The pass itself is `smediadrop::collectExternalMedia` in `app/media`,
beside `materialiseIntoProject` whose rules it inherits, so the button and the
`collect-external-media` test verb run the SAME code.

Three things it must keep doing:

* **Refuse on an untitled project**, with the reason. The destination is the
  project file's own folder and there is not one yet.
* **NOT save.** The references move in memory; writing the .qxp stays the user's
  decision. Since there is no dirty FLAG (`hasUnsavedChanges()` reads the undo
  stack's clean marker) and a collect is deliberately not an action — it copies
  files, and no undo step can put a file system back the way it found it —
  `QUndoStack::resetClean()` is how a non-action change says there is now
  something worth saving.
* **Name the placeholders it skipped.** A file this machine cannot see has no
  bytes to copy, and a collect that counted it as done would be a lie the user
  discovers only on the next machine.

**NOT gated (hand-verified only):** every widget in the banner — that it appears
and disappears, its wording, the button's connection, the confirmation and the
report dialogs, and the untitled-project refusal. There is no testkit verb that
builds the resources dock off screen, the same gap `assert-midi-options` fills
for the MIDI options page and nothing fills for the Audio one. What IS gated is
the NUMBER the banner is computed from (`assert-extern-files external=`) and the
pass the button runs (`collect-external-media`). Verified by hand on Linux
against a real project carried from Windows: `resources: project is NOT
self-contained — 3 file(s) outside the project folder` and `load: 3 sample
file(s) could not be found`, both from the production `openProjectFile` path.
