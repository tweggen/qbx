# app/testkit — CONTRACT

Purpose: the headless test harness — SActionScript (.qxa parsing),
SActionRunner (submit actions, per-action rejection accounting,
assertions), assert-audio-energy/peak, screenshot action, and the
roundtrip test main.

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
   are detectable by region RMS.
4. Exit code: 0 iff all actions applied as expected AND <assertions> pass.

How to test:
  cd smaragd/tests/cases
  ../../build/bin/smaragd.exe --test-case <case>.qxa --test-output-dir <dir>
  ../../build/bin/action_roundtrip_test.exe   # 2 pre-existing assert-action
                                              # serialization failures

Known debt: scripted toggle-playback segfaults (pre-existing); screenshots
need the window (not truly headless on all platforms).
