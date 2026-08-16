# app/testkit — CONTRACT

Purpose: the headless test harness — SActionScript (.qxa parsing),
SActionRunner (submit actions, per-action rejection accounting,
assertions), assert-audio-energy/peak/frequency, screenshot action, and
the roundtrip test main.

Public headers: app/testkit/*.h. Verb reference: docs/ACTIONS.md.

Depends on (engine): tw/analysis, tw/devices, tw/playback, tw/sinks
(+core/graph, and metering/pages/schedule/sidecar for the verbs that need
them). App edges: actions, model, objects/mixer, objects/track, shell.

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
   The assert-audio-* verbs, assert-channels-differ and assert-file-identical
   resolve their path attributes through resolveTestFilePath
   (app/testkit/stestfilepath.h): the test output dir first — a render, the
   normal case — then the .qxa's own directory, then the cwd. That is what lets
   an assertion be pointed at a committed FIXTURE (`../test_channels4.wav`),
   which never appears in the output dir. A name that exists nowhere still
   fails with the output-dir spelling, unchanged.
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
6. `channel=` means the channel, ALWAYS. It used to be dropped whenever
   `frameCount` was omitted (the whole-file path hard-coded the all-channels
   pooled figure), which was invisible while every channel of every render was
   equal and would have started mis-passing the day the sink goes wide. A
   channel index the file does not have is now an ERROR, not an empty selection
   reporting RMS 0 — that reads exactly like a silent render. Gates:
   channel_assert_fixture.qxa (every band there is chosen to EXCLUDE the pooled
   value, so the pre-fix code fails it) and channel_assert_dupmono.qxa.
7. assert-channels-differ measures two things in one pass — |rms(A) - rms(B)|
   (`minRmsDelta`, a LEVEL discriminator) and rms(A - B) (`minDiffRms`, a
   CONTENT one, off by default). The level test is what a duplicated bus fails;
   the content test is what catches two channels at the same level holding
   different audio. Today's sink duplicates, so channel_assert_dupmono.qxa
   asserts the failure via expectReject — and is SUPPOSED to break when the
   sink goes wide. ../test_channels4.wav is the asymmetric fixture it is proved
   against: 4 channels of a 480 Hz sine on a 6 dB RMS ladder
   (0.5 / 0.25 / 0.125 / 0.0625, pooled 0.28810), written and re-checked by
   tw303a/analysis/tools/gen_channel_fixture.cc (`--verify`).
7b. assert-clip-channels (proposal 36 B3) asserts a CLIP's page width, because
   no rendered file can show it before B5: the sink collapses the graph to one
   bus and duplicates it, so `assert-channels-differ` on a render measures the
   sink, not the clip. It resolves the clip through `SCut::resolveClip` — the
   same one-snapshot component+position fold `twView::freezePage` uses — and
   reads the page the RESOLVED component published, which is exactly the seam
   AC B3.2 is about. Two things separate it from assert-meter: it declares a
   demand on the project's `CaptureRevalidator` and waits (the SCHEDULER path,
   the one playback and render take) rather than driving the legacy pull, and
   it therefore needs a live revalidator — it REJECTS under
   `SMARAGD_REVAL_WORKERS=0` rather than quietly measuring something else. Same
   two discriminators as assert-channels-differ, same expectReject idiom, and
   an out-of-range channel is an ERROR (channelPtr clamps to 0, so accepting it
   would compare a channel with itself and read as "identical").
   ../test_stereo.wav is its asymmetric fixture: the 2-channel member of the
   same gen_channel_fixture ladder (rms 0.5 / 0.25), 144000 frames so it spans
   three pages and a page-DISPLACED channel cannot pass.
8. assert-file-identical is the byte gate. Render exactness has been `cmp`'d
   between runs since the beginning of this repo and never by the suite, so
   "byte-identical" could be claimed and not enforced. It must be proved to
   FAIL as well as pass, in each of its three shapes — size, content, missing
   reference — which is what file_identical_gate.qxa does; a gate never seen to
   fail is not known to be a gate.
9. report-page-memory REPORTS; it does not gate. Resident page count is a
   function of the readahead, the worker count and scheduler timing, so a bound
   tight enough to catch a regression would flake. render_duration_and_pages.qxa
   keeps a loose order-of-magnitude ceiling and proves the bound can reject
   (`maxPages="0"` after a render); the exact arithmetic is pinned by
   `ctest -R graph_test`, against a page count a unit test controls exactly.

  assert-meter (proposal 34) needs NO transport, which is the point: levels are
  read from frozen pages BY POSITION, so the verb freezes the page it asks
  about (`requestPage` — legal off the RT thread, deduped, the same call the
  offline render makes) and runs the production twLevelProbe. That keeps it
  deterministic under SMARAGD_REVAL_WORKERS=0 and independent of
  toggle-playback, which segfaults in scripts. CAVEAT: it drives the LEGACY
  PULL path, where twStreamingLatch::copyData gates on the twPluginChain's
  content epoch — which STrack::invalidateRenderPath() does not reach. So a
  track's gain must be set BEFORE the position is first probed; changing it
  afterwards is not observed here (playback and render both see it, because
  they go through the scheduler). meter_postfader.qxa therefore uses two tracks
  at different gains rather than changing one track's gain twice.

  assert-meter's LANE assertions (proposal 36 B8) are only meaningful in PAIRS.
  `expectLanes` pins the WIDTH and `laneA`/`laneB`/`minLaneRmsDelta` pin that
  the lanes hold DIFFERENT audio; either alone is passable by the wrong thing —
  a duplicated-mono meter satisfies every per-lane level bound, and a page that
  quietly stayed mono satisfies every bound on lane 0. Never make the delta
  claim over test_sawtooth.wav, whose two channels are byte-identical (proposal
  36 trap 22): meter_levels.qxa uses it as the negative CONTROL (two lanes,
  delta rejected) beside test_stereo.wav's 6 dB ladder. An out-of-range lane is
  an ERROR, never a silent rms 0.

  grabHead paints the REAL track head at a named lane height and column width.
  It pushes the measured level into the head's meter DIRECTLY
  (SSMVMixerControl::tkPushMeterLevel) rather than emitting a tick: a head built
  for a grab is never SHOWN, so onMeterTick's isVisible() gate — the first layer
  of the repaint-storm defence — would make every tick a no-op and the artifact
  would picture the layout with the thing under test sitting at the floor.

  dump-playback-capture is the only verb that asserts on the PLAYBACK path.
  Everything else in this suite reads a RENDER, which is a different consumer
  of the same graph, so nothing about AudioEngine::pullBlock, the readahead
  frontier, the seek on start or the stale-page fallback used to be covered at
  all — NullBackend::startOutput sets a flag and never calls the callback.
  A --test-case run now selects the CAPTURE backend (main.cpp sets
  SMARAGD_AUDIO_BACKEND=capture before SApplication exists, unless it is
  already set), which pumps the callback on a real-time paced clock and keeps
  every frame. Three rules for writing such a case:
   1. Captured frame 0 is the first frame of the CURRENT playback session, i.e.
      the locator position play started from (the recording is cleared at each
      start). Measured leading silence is zero, but budget for a bounded amount
      explicitly — do not assert content at frame 0 by luck.
   2. Playback is REAL TIME. A case's wall-clock cost is the span it plays;
      keep the spans short (SMARAGD_CAPTURE_SPEED can accelerate a smoke run,
      but the committed case must pass at 1.0x).
   3. Keep every position on the 4096-frame block grid when decoding with
      assert-source-position, and allow a lower minConfidence than the 3.0
      default: an underrun leaves a short zero gap that leaks energy into other
      bins, and the argmax is still the right block.

How to test:
  cd smaragd/tests/cases
  ../../build/bin/smaragd.exe --test-case <case>.qxa --test-output-dir <dir>
  ../../build/bin/action_roundtrip_test.exe   # 2 pre-existing assert-action
                                              # serialization failures

Known debt: screenshots need the window (not truly headless on all
platforms). The older "scripted toggle-playback segfaults" note is retired: two
cases now drive real playback through the capture backend
(playback_start_after_edit_position, split_plain_screenshot) and no crash was
observed over the repeat sweep. What still has NO bespoke gate is the capture
backend's own pacing and the latency of the playback path — a wall-clock
assertion tight enough to separate those behaviours would be flaky.
