# app/testkit — CONTRACT

Purpose: the headless test harness — SActionScript (.qxa parsing),
SActionRunner (submit actions, per-action rejection accounting,
assertions), assert-audio-energy/peak/frequency, screenshot action, and
the roundtrip test main.

Public headers: app/testkit/*.h. Verb reference: docs/ACTIONS.md.

Depends on (engine): tw/analysis (+core/graph). App edges: actions, model,
objects/mixer, objects/track, shell.

Invariants:
1. A rejected action FAILS the test unless its element has
   expectReject="true"; an expectReject action that APPLIES also fails.
   (Rejection detection samples SActionHistory::rejectedCount() around each
   submit — the drain is synchronous today.)
2. setup project="load" is NOT implemented — load real projects with the
   load-project ACTION.
3. Run from tests/cases/ so ../test_sawtooth.wav fixture paths resolve.
   The fixture is a 4 s ramped sawtooth: every source second has a unique
   RMS (sec0 .067 / sec1 .176 / sec2 .291 / sec3 .405) so wrong-offset bugs
   are detectable by region RMS, and it is strongly periodic at ~440 Hz so
   assert-audio-frequency can measure a transposition on it.
4. Exit code: 0 iff all actions applied as expected AND <assertions> pass.
5. drag-clip-edge is the ONLY route to clip-edge gesture code. Every clamp and
   snap of a trim / extend / loop / loop-marker drag lives in
   SMVActualView::mouseMoveEvent; resize-clip writes the window straight to the
   model and sails past all of it, so a resize-clip script can pass while the
   gesture is broken. It goes out through shell (SMainWindow::dragClipEdge) —
   testkit may not include app/timeline. The window is never shown in test mode
   and the runner never opens the project through it, so the first drag builds
   the arranger widget on demand; later drags in the same script share it.
   Modifier gestures ARE drivable (`modifiers="alt"`, `"ctrl"`, `"shift"`, or a
   "+"-joined combination): the handlers read ev->modifiers(), so a synthesized
   event carries its own state. Body gestures (slip, duplicate, move) need
   `edge="body"` — a press inside an edge band arms trim/extend instead, and a
   clip too narrow to have a body clear of both bands is rejected.
   Limit: the drop is quantised to a pixel at the view's zoom, so assert on
   ranges rather than exact frame counts.

How to test:
  cd smaragd/tests/cases
  ../../build/bin/smaragd.exe --test-case <case>.qxa --test-output-dir <dir>
  ../../build/bin/action_roundtrip_test.exe   # 2 pre-existing assert-action
                                              # serialization failures

Known debt: scripted toggle-playback segfaults (pre-existing); screenshots
need the window (not truly headless on all platforms).
