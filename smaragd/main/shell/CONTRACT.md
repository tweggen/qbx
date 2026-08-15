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

7. **Docks are created in the CONSTRUCTOR, with a stable objectName**, or
   `restoreWindowLayout()` cannot restore them (inv. 4). There are now six:
   `dock_extern_file_list`, `dock_track_detail`, `dock_log`,
   `dock_clip_properties`, and — since proposal 36 P4 —
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

How to test: full qxa suite (headless boots the shell); startup-layout
repro harness in STATE.md 2026-07-11.

Known debt: SApplication is the app-wide service locator — the SCC hub;
Phase 6 splits it into narrow context interfaces.
