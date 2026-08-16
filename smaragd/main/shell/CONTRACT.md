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

How to test: full qxa suite (headless boots the shell); startup-layout
repro harness in STATE.md 2026-07-11. The barrier specifically:
`qxa.instrument_render_determinism`, `qxa.instrument_render_determinism_xproc`
(a CMake driver running two processes through one output directory — the only
way to make the fresh-process byte compare the render gate rests on) and
`qxa.instrument_locate_continuity`.

Known debt: there is no UI for the per-track MIDI port — the verb and the serialized attributes exist, the arranger does not offer them yet (P4's track inspector work). Nor is there a UI for the `midi/portId/<name>` mapping: the Options page LISTS the machine's ports but a name that does not match one has to be mapped by editing `smaragd.ini`.

Known debt: SApplication is the app-wide service locator — the SCC hub;
Phase 6 splits it into narrow context interfaces.
