# app/testkit — CONTRACT

Purpose: the headless test harness — SActionScript (.qxa parsing),
SActionRunner (submit actions, per-action rejection accounting,
assertions), assert-audio-energy/peak/frequency, assert-file-identical,
assert-log, screenshot action, and the roundtrip test main.

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
9b. INSTRUMENTS ARE SILENT UNDER SMARAGD_REVAL_WORKERS=0, BY DESIGN (proposal
   37 P3b). `0` disables the revalidator and every consumer falls back to the
   LEGACY PULL, which is positionless — and an instrument cannot place a single
   event without a position, so `twPluginSlotProcessor::render(positional=false)`
   answers silence and logs once (`tw303a/plugins/CONTRACT.md` inv. 42). Two
   consequences for a case author:
     - an instrument case must never be run at worker count 0, and the race
       sweeps for `instrument_*` are over {1,4,8,16};
     - a verb that drives the legacy pull (assert-meter) reads SILENCE on an
       instrument track and is therefore not a way to measure one; use a render
       plus assert-audio-energy / assert-audio-frequency.
   `assert-instrument-slot` itself asks the MODEL and needs no render at all, so
   it works at any worker count including 0.

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
  toggle-playback, which segfaults in scripts. It drives the LEGACY PULL path,
  which is what the following used to say, and no longer does:

      CAVEAT (RETIRED by proposal 37 P3a): twStreamingLatch::copyData gates on
      the twPluginChain's content epoch, which STrack::invalidateRenderPath()
      does not reach, so a track's gain had to be set BEFORE the position was
      first probed — meter_postfader.qxa uses two tracks at two gains for
      exactly that reason.

  The fader is now twGainStage, sitting between the chain and the rewire, so the
  epoch the rewire's cached input page is gated on IS the one set-track-volume
  bumps: a gain change after a position was frozen is observed here. Gate:
  meter_gain_after_probe.qxa, which probes, sets the gain, and probes the same
  position again. (Measured honestly: that case also passes on the pre-move
  binary at the 36-B4 integration tip, so the caveat was already inert there —
  P3a is what makes it structurally impossible rather than accidentally absent.)
  meter_postfader.qxa keeps its two-track shape; it is a good case regardless.

  assert-file-identical is the byte-`cmp` determinism gate, inside a case.
  Until it existed that compare could only be run by hand from a shell, so no
  committed case carried it and every "the goldens did not move" claim in a PR
  body was a human's word. Absolute paths are ALLOWED (unlike `render`'s output
  name) precisely so a case can compare against a file another process wrote; a
  bare name still resolves in the test output directory. A frame range parses
  both files as RIFF/WAVE and compares only that slice of the `data` chunk —
  the files are 16-bit PCM, so this is a byte compare, never a float
  reinterpretation.

  assert-log is the ONLY way to gate a recovery. A recovery is exactly the case
  where the audio cannot tell you anything: a repaired project renders like a
  project that never needed repairing, so the WARNING is the evidence. There is
  no log file under `--test-case` (main.cpp skips the file sink deliberately —
  the suite would otherwise append to the one smaragd.log in the user's config
  dir and race over it under `ctest -j`), so the verb reads the in-process TwLog
  ring, which every channel funnels into. Two rules make it dependable: a
  `--test-case` run RAISES the ring capacity (a long render must not be able to
  evict the line under test before the assertion reads it — and the capacity is
  set exactly once, because setCapacity discards what is buffered), and the
  window is the records logged since the PREVIOUS action started
  (SActionRunner marks each boundary; an assert-log does not move the window, so
  two in a row examine the same action). Counting the whole run instead would
  make "twice" depend on everything that came before.

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

MIDI-out assertions (proposal 37 P7b):
  A --test-case run also selects the CAPTURE MIDI ports (main.cpp sets
  SMARAGD_MIDI_BACKEND=capture unless it is already set), for the audio
  backend's three reasons plus one: the capture port records
  {hostTimeNs, port, bytes} and NOTHING ELSE — specifically not the due time it
  was asked for, because the difference between the two IS the measurement.
  It reports supportsTimestamps() == false on purpose, so the recorded instant
  is when the message reached the wire rather than when a driver took it.
   1. THE MEASUREMENT IS INDEPENDENT OF THE PUMP. assert-midi-out maps a
      message's host time onto a project frame through the AUDIO capture
      backend's block log (CaptureBackend::frameAtHostTime) — a log written by
      a different thread that knows nothing of the pump. Asking the pump where
      it thought it was would prove nothing.
   2. `at` is SIGNED project frames since playback started, and it already has
      the device output latency subtracted, so it reads "the project frame
      whose audio was being HEARD when this left". An event with a positive
      per-track offset legitimately lands at a NEGATIVE `at`.
   3. SMARAGD_CAPTURE_SPEED must be 1. The audio log stays empirically correct
      at other speeds, but a project frame then means something different in
      wall-clock terms while the MIDI due times do not.
   4. Tolerance 4096 frames (~85 ms at 48 kHz) is the budget the design sets.
      Measured on this box the steady-state error is under 600 frames and the
      first event of a run under 400; a case that needs more than 4096 has
      found a bug, not a slow machine.
   5. An event whose offset-shifted due time falls BEFORE the run start is
      clamped — you cannot send a message before the transport started — so a
      case asserting an offset must place its notes far enough in.
   6. assert-midi-out REJECTS when the MIDI backend is not `capture`, rather
      than reporting "0 messages, all good": a silently passing assertion would
      make every MIDI-out case vacuous the moment someone ran the suite against
      a real device. `midi_out_backend_reject` is that gate.
   7. set-option writes the developer's REAL smaragd.ini. Only one case in the
      suite may own any given key, and it must set it back:
      midi_out_chase_and_stop owns `midi/chaseNoteOns`, midi_options_page owns
      `midi/outOffsetMs`. Both are RUN_SERIAL, because the pump reads those
      values at every transport start and a concurrent playback case would
      inherit them.

Event assertions (proposal 37 P1):
  assert-midi-events has TWO scopes and they are not the same object.
  scope="clip" reads the cut's own frame-domain snapshot - what the edit verbs
  move. scope="feed" runs STrack::eventFeed()->collect(), the merge of the
  track's own clip set with every child track that bubbles events up, which is
  the ONLY place mute, solo and midiRouting are observable and is what an
  instrument will read in P3b.
  A note-off is not in any table - notes are stored WITH their length - so a
  kind="noteoff*" assertion runs a real collect, over the clip's window PLUS
  ONE FRAME: windows are half-open and a release SYNTHESISED at the clip end
  lands on the boundary, i.e. in the window that STARTS there (events/CONTRACT
  inv. 8-9). kind="noteoff-synth" is what gates the non-destructive split.
  assert-clip-window is the geometry assertion the tempo work needed: a clip's
  placement and window in timeline frames, read through SClipWindow so it works
  for any window type. Before it, a script could only see a clip's position by
  rendering it, and a render cannot separate "the clip moved" from "the clip
  moved and its content moved back".
  assert-midi-file is counts and shape; byte identity of an SMF is
  assert-file-identical's job and is only a legitimate gate for a file twSmf
  AUTHORED (it has one canonical spelling, so a foreign file round-trips to an
  equal event TABLE, not to equal bytes).
  A .mid written by export-midi-file goes where the case says, verbatim: the
  path is NOT resolved against the script directory, because nothing may write
  into the shared tests/cases working directory (that is one of the properties
  that make the suite safe under ctest -j). Cases name ../../build/<case>.mid.

Event EDITOR gestures (proposal 37 P4):
  virtual-key, drag-note, assert-event-editor and assert-track-head all go
  through the SHELL (SMainWindow), because testkit may include neither
  app/eventui nor app/timeline (inv. 5) - the same route drag-clip-edge and
  assert-meter take to reach the arranger and the track head.
  They drive the REAL widgets. virtual-key presses the virtual keyboard, which
  submits `add-note` at the LOCATOR; drag-note synthesises press/move/release
  on SPianoRollView, which submits `set-notes` on release. Neither verb is
  undoable itself: the NESTED action is what lands on the stack, so
  `<undo count="1"/>` after the verb reverses the gesture - exactly the shape
  drag-clip-edge and plugin-editor-set-param already have.
  A drag-note drop is PIXEL-QUANTISED and then grid-snapped. At the arranger's
  default 30 px/s one pixel is 64 ticks and the 1/16 grid is 240, so a move of
  a BAR lands on the slot it aimed at with margin either side while a move of
  one division would not. Assert a grid position or a range, never an
  arbitrary tick.
  A note dragged ONTO the clip's window end disappears from the clip's
  snapshot - windows are half-open. That is the window's rule, not the
  gesture's; give the case a clip long enough that the destination is inside.
  assert-event-editor's `contains` matches a CONTIGUOUS substring of
  describe(), so the field ORDER of `kind=…|notes=…|grid=…|linked=…|empty=…`
  is part of the contract. assert-track-head reads describeHead(), whose
  fitW/fitH fields are the "hiding beats clipping" density rule made
  assertable. PNG grabs are coverage of the paint paths, never oracles.

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
