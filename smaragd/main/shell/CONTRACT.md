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

How to test: full qxa suite (headless boots the shell); startup-layout
repro harness in STATE.md 2026-07-11.

Known debt: SApplication is the app-wide service locator — the SCC hub;
Phase 6 splits it into narrow context interfaces.
