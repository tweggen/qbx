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
3a. AUDIO INPUT is `null` by default under --test-case (proposal 21 L0;
   main.cpp sets SMARAGD_AUDIO_INPUT_BACKEND next to the audio and MIDI backend
   defaults, unless it is already set). THIS CHANGES NOTHING ABOUT THE SUITE AS
   IT STANDS: no case records audio — there is no record verb yet — so nothing
   opened an input device in the first place. It is set now so that the moment
   one does, it cannot reach the developer's microphone by accident, and so a
   case that WANTS input says so: `SMARAGD_AUDIO_INPUT_BACKEND=file:<wav>` gives
   a device that replays known audio in 1024-frame blocks on the shared steady
   clock (devices inv. 22-23).
   The MIDI-in verbs (`midi-in-event`, `midi-in-replay`) inject into the CAPTURE
   MIDI input, which is already the --test-case default. Nothing CONSUMES a
   MidiInput yet (L1a/L3a attach the live lane and the recorder), so today those
   verbs gate their own behaviour — order, stamps, real-time pacing — and
   nothing sounding.
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
   different audio.

   **channel_assert_dupmono.qxa is NOT the sink gate it was designed as, and
   this invariant said it was until proposal 36 B9.** It was written at M0 with
   an `expectReject` that was "SUPPOSED to break when the sink goes wide". The
   sink went wide at B5 and it did not break — because its fixture is
   `test_sawtooth.wav`, a two-channel file whose channels are BYTE-IDENTICAL
   (§7 trap 22), so its render has equal channels whether the engine is wide or
   not. A gate whose fixture cannot distinguish the two states is not a gate for
   that distinction. It still earns its place as the EQUAL-channels half of a
   pair — see `meter_levels`, which asserts equal channels on the sawtooth and
   different ones on `test_stereo.wav`, and which only a real per-channel path
   passes both halves of.

   ../test_channels4.wav is the asymmetric fixture it is proved
   against: 4 channels of a 480 Hz sine on a 6 dB RMS ladder
   (0.5 / 0.25 / 0.125 / 0.0625, pooled 0.28810), written and re-checked by
   tw303a/analysis/tools/gen_channel_fixture.cc (`--verify`).
7b. assert-clip-channels (proposal 36 B3) asserts a CLIP's page width. It was
   written because no rendered file could show one: before B5 the sink
   collapsed the graph to one bus and duplicated it, so `assert-channels-differ`
   on a render measured the sink rather than the clip. B5 widened the sink, so a
   FILE can show channels now — and this verb is still the only place a CLIP's
   OWN width is observable, because everything between the clip and the file
   (track mix, gain stage, rewire, master) can change it. It resolves the clip through `SCut::resolveClip` — the
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
   1. Captured frame 0 is the first frame of the CURRENT DEVICE session
      (`CaptureBackend::openDevice` clears the recording; proposal 21 L1a,
      design D5). With NO LIVE LANE — which is every case today — the device is
      still opened at play and closed at stop, so this is exactly the old
      "current playback session" and every existing `dump-playback-capture`
      case reads the same recording it always did. It stops being the same
      thing once a track is armed: the device then outlives the transport, and
      a second Play must NOT erase what the monitored input recorded in
      between. Measured leading silence is zero, but budget for a bounded
      amount explicitly — do not assert content at frame 0 by luck.
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

Automation UI gestures (proposal 37 P6):

  1. `drag-automation-point` extends inv. 5, it does not sidestep it. It goes
     out through `SMainWindow::dragAutomationPoint` because testkit may not
     include app/timeline, works out where the addressed breakpoint IS on
     screen from the lane's own value scale, and sends REAL press/move/release
     events into the arranger canvas. What runs is
     `SAutomationLaneUi::press/move/release`, not a re-spelling of them.

  2. It is NOT undoable itself. The gesture submits its own verb —
     `add-automation-point` on a click over empty lane space, one
     `move-automation-point` on a drag, `remove-automation-point` on a
     primary-click, `set-automation-points` on an Alt-drag — and THAT is what
     `<undo count="1"/>` reverses. A click that also submitted the move of the
     drag it arms would be two steps; the release only submits a move when the
     point actually moved, which is what keeps a click at exactly one.

  3. The lane has to be SHOWN first (`set-lane-view showAutomation="1"`), and a
     `cut:Gain` target additionally needs `clipEnvelopes="1"`. Both are VIEW
     state and neither creates anything in the model — a shown-but-empty lane
     draws its default value and the first gesture is what brings the model
     lane into existence. A `cut:` gesture with envelopes disarmed is REJECTED
     rather than silently claimed, because there the CLIP owns the press.

  4. The drop is pixel-quantised and then grid-snapped, exactly as for
     `drag-clip-edge` and `drag-note`. TIME is nevertheless exact when the case
     aims at a grid multiple (24000 frames at the default 120 BPM); VALUE is
     quantised to one pixel of the lane, so a value assertion needs a tolerance
     and the case has to say what a pixel is worth on that lane. Do not fix a
     value assertion by widening it past the point where a wrong SCALE would
     still pass.

  5. `automation-write-tick` is the `slip-clip` shape: it feeds
     `SAutomationRecorder` and pushes no undo step, exactly as a fader moving
     mid-pass does. The pass commits ONE `set-automation-points` when it ends,
     and that single action is the undo step — which is the property
     `automation_write_pass.qxa` exists to pin. It REJECTS on a lane that is
     absent or not in touch/latch/write, so a case cannot pass by ticking into
     the void, and `release="1"` on no open pass is likewise a reject.

  6. Give `automation-write-tick` an explicit `time`. It defaults to the live
     locator, which is what a real fader does, but a case that let it default
     would be measuring this machine's scheduling rather than the recorder:
     `wait-playhead` is what makes the tick land during genuine playback, and
     `time` is what makes the resulting POINT exact.

  7. `assert-lane-alignment` now covers automation sub-lanes for free (they are
     sub-lanes by the same rule take lanes are) plus two things only an
     automation row can get wrong: hanging off a lane group that is not its
     track's, and naming an owner the model cannot resolve. Its `grabPng`, and
     `assert-track-head`'s, are COVERAGE of the paint paths and never oracles —
     the head grab is the only thing that paints the "A" button's per-mode
     colour, and the canvas grab the only thing that paints a lane at all.

  8. NOT reachable from a script: the Delete key over a marquee selection.
     Delete is a QAction SHORTCUT (`actRemoveSample_`), not an event a case can
     synthesise, so the marquee gesture is gated and the deletion it enables is
     not. Recorded rather than papered over.

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

## `assert-automation-value` (proposal 37 P5)

It asks the SNAPSHOT the engine is handed (`SAutomationLane::valueAt`), never a
re-implementation of the interpolation, so a case can pin the midpoint of a ramp
as a closed form without also rendering it. That is deliberately a SECOND,
independent view of one curve: the rendered RMS bands say the curve reached the
audio, this says the curve is the one the script asked for, and a bug that moved
both would have to move them consistently.

A MISSING lane is REJECTED rather than reported as the target's default value —
a typo in a `target` must never read as a passing assertion. Pair it with
`expectReject="true"` to assert that a lane is ABSENT.

## The live-lane assertions (proposal 21 L1b)

10. **The measurement is independent of the thing measured**, the same
    discipline the MIDI-out assertions follow and the only reason any of these
    are worth anything. The INPUT is a committed WAV replayed by
    `FileAudioInput` through a real capture thread and ring
    (`SMARAGD_AUDIO_INPUT_BACKEND=file:…`, set on the CTest entry because the
    runner reads it long after the script is parsed); the OUTPUT is the audio
    capture backend's own recording of what the device was handed. Nothing in
    between is asked what it thinks it did.

11. **`assert-monitor-latency` is a cross-correlation, not a position decode.**
    `decodePositionAt` resolves 4096-frame blocks and the whole monitoring
    budget is 8192, so the decoder could not tell a pass from a fail. The
    correlation is NORMALISED — the peak is a similarity, so `minCorrelation`
    means the same thing whatever the monitored level is — and the input index
    WRAPS, because the file input loops.

12. **`assert-render-policy` bounds are MAXIMA and default to 0.** A non-zero
    bound is legal but must be spelled out in the case and justified there. All
    five L1b cases assert 0/0, and that is a property of the arm/disarm
    ordering (shell inv. 13), not luck: the wrong order measured 8.

13. **A case that arms and disarms is a sequence of DEVICE SESSIONS.**
    `CaptureBackend` clears its recording at device start (rule 1) and
    disarming the last live lane closes the device, so a disarm between phases
    makes frame 0 of the next dump the first frame of that phase — which is
    what lets a monitoring case use absolute frame windows instead of
    wall-clock guesses.

## The recording verbs (proposal 21 L3b)

14. **`record-start` / `record-stop` are ABSOLUTE and go through the production
    entry points** (`SApplication::startRecording` / `stopRecording`), which is
    what the record button calls — a script exercises the real path rather than
    a copy of it. `record-start` is REJECTED when nothing is armed or the input
    will not open; `record-stop` is REJECTED when not recording. Neither is
    undoable: they are transport. The PLACEMENT they cause is one undo step.

15. **A take that PUNCHES OUT ends without `record-stop`**, so a case that sets
    a punch region must not call it afterwards (it would be rejected). The
    dialog, the record button and the punch-out all converge on the same
    `stop()`.

16. **`assert-recorded-clip`'s numeric expectations are opt-in**, so one verb
    serves the mid-take assertions (`growing`, `previewNonEmpty`,
    `minDurationFrames`) and the post-take ones. It ALSO checks the placement
    IDENTITY on every call once the take is over — the clip's link start must
    equal `placementFrame(trimmed)` — so a recorder that reported the right
    terms and applied different ones fails even when no expectation was given.

16a. **A CASE THAT ASSERTS `userOffsetFrames` MUST ALSO ASSERT
    `inputDevice`.** The offset comes from
    `SSettings::recordingOffsetMs(<the recorder's resolved input device>)`, and
    a lookup that MISSES returns 0.0 — indistinguishable from a device
    calibrated to zero. So `userOffsetFrames="0"` is not an assertion at all
    without the device name, and a non-zero expectation fails with an error
    about arithmetic when the fault is the key. Name the device with
    `set-track-input` (the first rung of the resolution), key the option under
    that name, and assert it back with `inputDevice=`.

17. **WHAT A RECORDING CASE CAN AND CANNOT CLAIM.** `P0`, the project frame
    capture frame 0 maps to, depends on when the capture thread and the render
    callback actually ran; it is NOT predictable and no case asserts it. What
    IS a closed form in integers is the COMPENSATION — `-inputLatency -
    outputLatency + userOffset` — because a case sets the reported input
    latency (`SMARAGD_AUDIO_INPUT_LATENCY_FRAMES`) and the capture backend's
    output latency is its own buffer (1024). `FileAudioInput` does NOT actually
    delay by the reported latency, which is the point: the gate is on the
    CONVERSION, not on the physics of a device no headless run can see.

18. **`sourceAtStartFrame` is a FAITHFULNESS claim, not a latency one.** It
    decodes the position-encoded fixture at the placed clip's first content
    frame and compares it to the number of capture frames the mapping says were
    trimmed. They agree only if every frame the file produced reached the pages
    in order, none lost or duplicated between the device ring, the bridge
    thread, the growing capture source and the placement. The tolerance is one
    DECODE block (4096) because the decoder resolves 4096-frame blocks by
    construction.

19. **A LOOP-PASS COUNT IS A WALL-CLOCK QUANTITY.** It is captured material
    divided by loop length, and captured material shrinks when the box is
    loaded enough to cost ring overruns — `record_loop_takes` DID fail under a
    concurrent suite with an exact count. Assert a FLOOR (`minTakes` /
    `minPasses`) and let the verb assert the part that is not load-sensitive:
    ONE COLUMN, and exactly as many takes as passes, which it does
    unconditionally whenever more than one pass was committed.

20. **Nothing undoable may come between a take and the `<undo/>` that gates
    it.** A `select-take` probe placed there is undone instead, which is how
    the first draft of `record_loop_takes` mis-read a working undo as a broken
    one.

## Live instruments (proposal 21 L2)

14. **`assert-audio-onset` REPORTS the number it measured, pass or fail.** The
    acceptance criterion asks for the onset lag to be RECORDED, not for a
    boolean, so the verb prints "onset lag N frames (M ms)" on every run. The
    scan is a running RMS window rather than a per-sample threshold: one sample
    above the line is a click or a dither bit, and an onset a case can reason
    about is where ENERGY begins. `afterMidiIn="1"` measures RELATIVE to the
    last injection by mapping its host time through
    `CaptureBackend::frameAtHostTime` - the AUDIO backend's own block log,
    which shares no code with the pump, the ring or the event source. No
    latency term belongs in that comparison: the capture dump IS what the
    device was handed, and `frameAtHostTime` answers in exactly that domain.

15. **`assert-render-policy` grew a MINIMUM, and only one case may use it.**
    `minLiveOwnedRefusals` exists for `live_instrument_ownership`, which
    asserts that the ownership guard FIRED. "At most 0" and "at least 1" are
    different claims; the minimum defaults to 0 so every other case means
    exactly what it meant.

16. **`assert-midi-out maxLagMs` is HOST TIME TO HOST TIME**, with no frame
    mapping anywhere in it - unlike `at`, which maps through the audio clock.
    Thru has no position: it is a key being pressed right now, and the only
    question is how long the wire took. Both sides are recorded by things that
    are not the thing under test (`midi-in-event`'s injection instant and the
    capture port's `send()` instant), which is what makes the number worth
    anything.

17. **`virtual-key` has TWO MODES and a case must choose.** `hold`/`release`
    PLAY the computer keyboard's MIDI port; the default WRITES a note at the
    locator. The real mouse handler does both, because a user pressing a key
    means both - but a case measuring what an instrument SOUNDS must not also
    be editing the project under the measurement, so the verb keeps them apart.
    `key="-1"` with `release` lets go of everything.

18. **`assert-midi-recorded` asserts SHAPE, not placement** (proposal 21 L4).
    Where a recorded note landed is `assert-midi-events`' job, over the clip's
    own frame-domain snapshot, per note, with a tolerance — a far better
    instrument than a second one that would have to re-derive the same domain.
    What this verb checks is how many event columns the lane holds, how many
    takes are on the first of them, and — from `SMidiRecorder` itself — how many
    passes, notes and non-note events were committed, in which mode and under
    which quantise grid.

    Reading BOTH sides is the point. A recorder that reports three passes while
    the lane holds one take is exactly the failure "one take per pass" is a
    claim about, and neither number alone can see it — so the verb asserts that
    identity UNCONDITIONALLY whenever more than one pass was committed, as
    `assert-recorded-clip` does for audio.

19. **A MIDI pass COUNT is wall-clock, a NOTE count is not.** How many cycle
    passes a take produces is elapsed time over loop length, so a loop case
    asserts `minPasses` / `minTakes`. How many notes arrived is a property of
    the performance being replayed, so `notes` can be exact. The same split
    `assert-recorded-clip` documents, for the same reason.

20. **`midi-in-replay` WITHOUT `startFrame` exercises the RETROSPECTIVE
    mapping.** It begins the instant the take does — while the readahead is
    still priming and the RT thread has published nothing — so the recorder
    buffers the host times and maps them at the stop, backwards through whatever
    anchor exists by then (design D6). Notes whose mapping lands before the pass
    are CLAMPED into it, so a case in that shape asserts COUNTS.

    WITH `startFrame` the performance is held until the playhead is running and
    the placement becomes a closed form — with a SYSTEMATIC OFFSET that is the
    conversion working rather than an error: `startFrame` waits on the PUBLISHED
    locator while the recorder maps to the frame being HEARD, and the published
    position leads the heard one by one device buffer plus the output latency.
    Measured on the capture backend: **-1025 frames**, inside
    `midi_record_placement`'s 4096 band.

21. **`assert-metronome-clicks` gates a SEQUENCE, not an onset** (proposal 21
    L5). `assert-audio-onset` answers "when did sound start", which is right for
    one note; a metronome is a GRID, and the claim worth gating is four
    properties at once — N clicks, on the beat, with silence between them, one
    in every bar louder.

    Three things about how it measures, each of which is the difference between
    a gate and a coin toss:

    - **The grid is anchored on the FIRST DETECTED ONSET.** Capture frame 0 is
      the DEVICE start, and how many blocks the ring took to fill before the
      first entry was summed is a property of the box. The SPACING is not — it
      comes from the tempo map — so that is what `intervalFrames` bounds.
    - **The accent PHASE is searched, and the FIRST onset is excluded from the
      level comparison.** Which beat of the bar the first summed entry carries
      is again the box's business; and the first click of a live-lane session
      sits inside the RT's 2-3 ms fade-in ramp and is attenuated BY CONSTRUCTION
      (measured 0.2109 against a full 0.4720). Its POSITION is still the anchor;
      only its level is not a claim.
    - **`silenceMaxRms` measures the MIDDLE of each gap.** Without a silence
      assertion "eight clicks" is satisfied by a continuous tone with eight
      louder moments in it; measuring the whole gap would fail on the click's
      own decay tail.

    `count` is EXACT and `minCount`/`maxCount` are the bounded form. Only a
    COUNT-IN can claim an exact count, because its click range is closed by
    POSITION; a run bounded by a wall-clock `wait-ms` produces a count that
    depends on the box.

22. **The transport-polish cases own two `smaragd.ini` keys between them.**
    `record_count_in` writes `transport/countInBars`, `record_pre_roll` writes
    `transport/preRollBars`, and each puts its own key back — the same ownership
    convention `midi_record_modes` and `record_offset_zero` follow, and the same
    `RUN_SERIAL` that makes it safe. No other case reads either key.

23. `assert-envelope` (proposal 39 M1) is THE one way a script reads a DRAWN
    envelope, and it reads it through the object's own INLINE RENDERER
    (`SObjectRenderer::collectEnvelope`), never through `SObject::getPreview()`.
    The difference matters: a cut's slip, its stretch and its loop tiling live
    in `SCutRendererInline`, so a verb that went straight to the preview would
    be a second implementation of all of them — free to agree with itself while
    disagreeing with every pixel on screen.

    It goes out through `SMainWindow::collectClipEnvelope`, the same routing
    inv. 5 gives `drag-clip-edge` and `assert-track-head`. Note what it does NOT
    need: no arranger and NO PAINTER. A collect is expressed on a time window
    (`SEnvelopeWindow`), so nothing here constructs a `QPainter` over a scratch
    image in order to ignore it.

    `snapshot` / `compareTo` store a probe array under a name and later assert a
    BYTE-IDENTICAL match. That pair is the point of the verb: it lets a case say
    "this edit did not move one byte of the waveform" without hard-coding a
    single expected probe. The table is process-global and lives for the run —
    one .qxa script per process.

    `mode` has two values and any other is REJECTED rather than silently treated
    as `clip`. `clip` reads ONE clip's own probes over `clip=`. `mode="childSum"`
    (proposal 39 M3) reads the FOLDER-SUM OVERLAY of the lane at `trackPath=`,
    through `SMainWindow::collectTrackChildSumEnvelope` ->
    `STrack::collectChildSumEnvelope` — which is the exact call
    `STrackRendererInline::draw()` makes to paint it, for the same reason clip
    mode goes through the renderer. Everything else on the verb (`start`,
    `length`, `width`, `column`, `min`/`max`, `tolerance`, `expectEmpty`,
    `snapshot`, `compareTo`) means what it means in clip mode, so the
    byte-identity pair is what gates "the folder's own fader moved nothing".

24. `assert-lane-overlay` (proposal 39 M3) is the PIXEL gate on that overlay,
    and it is the first verb in this repo that measures the arranger CANVAS's
    paint at all — the `screenshot` verb grabs a root window that is blank under
    `QT_QPA_PLATFORM=offscreen`, and `assert-lane-alignment grabPng=` writes a
    PNG nobody asserts on.

    It grabs the real canvas off screen (`SMainWindow::describeLaneOverlay`,
    over `grabArrangerLanes`' sizing dance) and classifies every pixel of the
    named lane's own band — inside the two 1 px separator lines the canvas draws
    — against two references it does NOT read off the image: the LANE FILL
    (`STrackRendererInline::laneFillColor()`, i.e. what the renderer itself
    would fill with) and the CLIP BODY (`QColor(160,160,160)`).

    **An overlay pixel is DEFINED as one strictly lighter than the fill and
    strictly darker than the clip body**, which is design D4's relation stated
    as a measurement. Draw the overlay darker than the lane, or as light as a
    clip, and the count falls to zero and the assertion fails; the two wrong
    directions are counted separately (`darkerThanFill`, `lighterThanClip`) so
    a failure says which way it went. `expectOverlay="false"` is the negative
    control and needs a lane that is bare — a lane holding a CLIP is not one,
    because the anti-aliased edges of the file name drawn on that clip land at
    every luminance between the text and the clip body, including inside the
    band.

    Two pieces of chrome are handled by identity rather than by luminance: the
    PLAYHEAD (`QColor(30,200,30)`, whose luminance falls inside the band) is
    counted in its own bucket, and the TIME GRID is not handled at all — run the
    verb after `grid-disable`.

25. `collapse-track` (proposal 39 M3a) folds a folder lane SHUT, or opens it
    again. VIEW state, exactly like `set-lane-view`: not undoable, saved
    nowhere. It exists because the folder-sum overlay is SOLD on the collapsed
    folder — fold it shut and you can still see what is under it — and nothing
    in the testkit reached the fold at all, so inv. 24's pixel gate necessarily
    grabbed an EXPANDED folder.

    It goes out through `SMainWindow::setTrackCollapsed` to
    `SStdMixerView::toggleTrackCollapsed()`, the same call the head's fold
    triangle makes (`ssmvmixercontrol.cpp`), rather than to a second writer of
    the collapsed set: that one call owns the row rebuild and the control
    column, so a second spelling of "collapsed" would be free to skip the half
    of a fold that anyone can see.

    **`collapsed` is ABSOLUTE, never a toggle.** A script that says what it
    wants is idempotent and can be read without counting how many times it ran.

    **There is no row-count probe and this verb does not invent one.** What is
    observable after a fold is that the children's rows cease to exist, so every
    lane BELOW the folder moves up by that many rows — which
    `assert-lane-overlay`'s own report line already carries as `row=N`, and
    `contains="row=N"` reads. The folder's own lane is painted by the same
    renderer either way, so the same verb on the folder still finds the overlay,
    and `assert-envelope mode="childSum" compareTo=` still reports the same
    bytes: collapsing is view state and may not move one probe of what the
    overlay describes. That trio is the user story, and it is what
    `folder_sum_preview.qxa` asserts.

26. **A case that reads an envelope must put its clips and its window somewhere
    other than frame 0**, or it cannot see the commonest way the collect can be
    wrong. `SCutRendererInline::collectEnvelope` clamps a negative clip-relative
    position to 0, so a clip handed a window that begins to its LEFT does not
    decline and does not shift — it stretches its whole content across every
    column. Every case written before 2026-08-18 (`envelope_probe`,
    `preview_volume_independent`, `folder_sum_preview`) starts every clip at 0
    and every window at 0, which is exactly the configuration in which the wrong
    answer and the right one coincide, and all three PASS on a binary with the
    per-clip span removed. `envelope_offset_window.qxa` is the one that does not:
    clips at 96000 and 384000, windows at 96000 and 192000, a two-second GAP
    whose columns must read exactly 0/0, and a column-per-second layout that puts
    every boundary on a whole second of the ramp so each expectation is a closed
    form at tolerance 0. Reverting the span fails 18 of its 26 childSum
    assertions.


## The media-browser verbs (proposal 38 gate 2)

Six verbs — `media-browser-source`, `-path`, `-search`, `-filter`, `-drag` and
`assert-media-browser` — driving the REAL `SMediaBrowserPanel`. They reach it
through `SMainWindow` for the standing reason (inv. 5: testkit may not include
`app/timeline`) plus one that is specific to the drag: it has to reach BOTH the
panel and the arranger, and the shell is the only module that sees both.

27. **The panel is built in the ctor and NEVER shown in a headless run**
    (proposal 38 trap T10), so every verb pushes it its state explicitly and
    `SMediaBrowserPanel::describe()` is the only oracle there is. Nothing in
    these verbs may depend on the widget having been painted or laid out.

28. **NOTHING SLEEPS.** The provider ABI is async by construction (app/media
    inv. 1), so every verb that issues a request waits for the panel to go IDLE
    — no live root request, no pending lazy expand, no search still inside its
    250 ms debounce — up to `waitMs`, and FAILS on the timeout. A case that
    slept for a fixed time would flake under `ctest -j4`, which is exactly the
    load these bounds meet. `waitMs="0"` means "issue and return", and it is
    what lets `media_browser_search` stack three searches to gate supersession.

29. **A SYNTHETIC DROP IS THREE EVENTS, NOT ONE.** Qt discards a bare
    `QEvent::Drop`: `QApplication::notify` tracks the drag TARGET a `DragEnter`
    established, and a Drop with no active target never reaches
    `QWidget::event` — measured, and true whether or not the widget is visible
    (a `Qt::WA_DontShowOnScreen` `show()` does not help). `mediaBrowserDrag`
    therefore sends DragEnter, DragMove and Drop in order. That is also better
    coverage: `dragEnterEvent` and `dragMoveEvent` are the handlers that decide
    whether the arranger accepts this MIME type at all.

30. **The three cases OWN the four `media/*` keys and are `RUN_SERIAL`.** The
    panel PERSISTS `media/lastSourceId`, `media/lastPath/<sourceId>`,
    `media/categoryMask` and `media/searchRecursive`, and suppressing that under
    `--test-case` was considered and rejected (design AC 9) — it would make the
    gate test something other than the shipping code. Each case declares the
    ownership in its header and restores all four to their `SOpt` defaults, so
    `smaragd.ini` comes back byte-identical. This is the `midi_options_page`
    precedent, and the residual hazard is the same one the audited `smaragd.ini`
    row already names: the CONVENTION, not the locking. A future case that READS
    one of those keys would be racing one that writes it.

## The gate-3 drop verbs (proposal 38)

31. **`media-test-source` configures a provider that only exists under a knob.**
    `SDelayedLocalSource` is registered as `testdelay` only when
    `SMARAGD_MEDIA_TEST_SOURCE=1`; the verb REFUSES when it is not there rather
    than silently doing nothing, because a case that configured no provider and
    carried on would then be measuring the wrong one. Its `clearCache="1"` is
    itself refused unless `SMARAGD_MEDIA_CACHE_DIR` named the cache root — a
    case run without the knob must not be able to wipe a developer's real cache.

32. **`media-drop-wait` waits on a CONDITION, never on a clock.** It pumps the
    event loop until `smediadrop::pendingCount()` reaches zero, up to `waitMs`,
    and a timeout is a real failure. `placed` / `failed` / `abandoned` /
    `fetches` are then asserted EXACTLY, and the three counters are three
    different claims: `abandoned` is "a fetch landed and DELIBERATELY placed
    nothing" (the target track was gone, or the project changed), which is a
    stronger statement than "no clip appeared".

33. **`media_drop_deferred` writes `media/lastSourceId` and
    `media/lastPath/testdelay`** and is `RUN_SERIAL`. It restores both, so the
    INI comes back with identical CONTENT. `media/lastPath/testdelay` is
    exclusively its own; `media/lastSourceId` is shared with the three gate-2
    cases, all four of which are `RUN_SERIAL` and all four of which restore it
    to `"local"`, so no run can observe another's value. It deliberately touches
    neither `media/categoryMask` nor `media/searchRecursive` — the defaults are
    what it wants, so there is nothing to restore.

    Note on the phrase this repo has used elsewhere: the INI comes back
    **identical in CONTENT**, which is the claim worth making. `QSettings` does
    not promise to preserve section ORDER across processes, so an md5 can
    legitimately move while nothing a case wrote has changed.

34. **A deferred placement is asserted on the TRACK it names, not on a clip
    index shared with another concurrent drop.** Two concurrent deferred
    placements land in FETCH-COMPLETION order, which is not the order they were
    dropped in and which nothing promises. Asserting `clip 0,0 startTime=48000`
    with both drops on one track failed 4 runs in 10; one clip per track states
    the real claim and is exact on every run.


## The Nextcloud accounts verbs (proposal 38 GATE 5b)

`set-media-account`, `remove-media-account`, `media-test-connection`,
`media-account-redaction-drive`, `assert-media-options` and
`assert-settings-file` — driving `SMediaAccountManager`
(`SApplication::mediaAccounts()`) and `SOptionsDialog`'s Media page directly.

35. **The CRUD verbs bypass the WIDGET, not the CODE PATH.** `set-media-account`
    / `remove-media-account` call `SMediaAccountManager::setAccount()` /
    `removeAccount()` directly — the SAME calls `SOptionsDialog::
    onMediaSaveAccount()` / `onMediaRemoveAccount()` make — rather than
    synthesizing a button click, exactly the `set-option` convention (inv. 30's
    sibling verbs synthesize a real drop for the SAME reason a widget's own
    geometry is part of what is under test there; an account's validation has
    no such geometry, so bypassing the widget loses nothing).

36. **`media-test-connection` and `media-account-redaction-drive` start their
    OWN throwaway `SWebDavStub`**, bound to `127.0.0.1:0` like every other use
    of that stub in this tree (never a fixed port — the whole reason `ctest -j4`
    can run four processes without a port collision). Neither needs the
    `media-webdav-stub` verb gate 5c adds: that one starts a stub for the
    DOCK to browse against across several actions in one script, which needs a
    port a later action can address; these two are each a single self-contained
    action and start, use and stop their stub inside ONE `apply()` call.

37. **`media-account-redaction-drive` is ONE action on purpose**, not a
    sequence of `set-media-account` + two browse verbs, because `assert-log`'s
    window is "since the action immediately before it started" (`sassertlogaction.h`):
    spreading the drive across several actions would put only the LAST one
    inside the window an immediately-following `assert-log` reads.

38. **`assert-settings-file` reads the ON-DISK spelling, which is NOT the
    logical `SSettings` key.** `QSettings::IniFormat` brackets only the FIRST
    `/`-separated component as a `[group]` header; everything below that is one
    key with the remaining `/`s written as `\`s — `"media/nextcloud/qxatest/
    url"` lands under `[media]` as `nextcloud\qxatest\url=...`. A case that
    wants to scope a check to one account writes the disk-spelled prefix itself
    (`nextcloud\qxatest\`) rather than passing a `section=` — a "[section]"
    header scoping scheme was tried and reverted (proposal 38 GATE 5b) because
    it can never match below the top group and would make every scoped
    assertion pass vacuously.

39. **The two OPTIONS-PAGE cases (`media_options_page`, `media_secret_redaction`)
    override `SMARAGD_SECRET_BACKEND=dpapi`** (set in `smaragd/CMakeLists.txt`,
    not in the `.qxa`, because the runner reads it before any script is
    parsed) and are `RUN_SERIAL`, owning and restoring every key under
    `media/nextcloud/qxatest/*` — the `midi_options_page` precedent, inv. 30's
    sibling. `media_options_page` additionally calls `media-browser-source`
    (inv. 27's verb), which persists `media/lastSourceId` as a side effect
    (design AC 9), so it owns and restores that key too. `media_options_no_store`
    forces `SMARAGD_SECRET_BACKEND=none` instead, in ITS OWN CTest entry — the
    backend is read once per process, so seeing AC 10's "Remember disabled"
    behaviour needs a whole separate process, not a mid-script switch.

## The end-to-end WebDAV verb (proposal 38 GATE 5c)

`media-webdav-stub` — the PROCESS-OWNED `SWebDavStub` the two end-to-end cases
(`media_webdav_browse`, `media_webdav_drop`) browse and download from. This is
the coverage gate 4 deliberately does not have: at the end of gate 4 the WebDAV
client was unit-tested and had never been driven from the app (gate 4 AC 9).

40. **THE CASE NEVER LEARNS THE PORT, AND MUST NOT. THE VERB WRITES THE URL
    WHERE THE FLOW READS IT.** The stub binds `127.0.0.1:0` — an OS-assigned
    port, never a fixed one, which is the whole reason four concurrent
    `ctest -j4` cases cannot collide over it — and a `.qxa` is a static document
    that cannot interpolate a run-time value into a later attribute. So given an
    `accountId=`, this verb calls `SMediaAccountManager::setAccount()` ITSELF
    with the stub's own `baseUrl()`, which is the same production path the
    Options dialog's Save button calls; the case then names only the SOURCE ID
    `nextcloud:<accountId>`, which is static. Two alternatives were considered
    and are worse: a fixed port re-introduces exactly the collision the
    OS-assigned one removes, and printing the port into a log line the case
    greps would let a case READ the port and still not be able to USE it.
    Consequence to keep in mind when reading either case: the URL appears in no
    `.qxa` at all, so `nextcloud:qxastub` is the only handle on the server.

41. **ONE STUB PER PROCESS, PARENTED TO `qApp`.** A `QTcpServer` torn down after
    `QCoreApplication` has gone is a teardown crash with a network-shaped delay
    on it — the same instinct that makes `SWebDavMediaSource` abort every reply
    in its destructor (§B.7), applied to the other end of the wire. Held as a
    `QPointer` so the qApp deletion is observable here rather than dangling.
    `action="stop"` is idempotent, which is what makes a defensive stop free.

42. **`fixtureDir=` mirrors REAL BYTES, and that is what makes AC 19 an audio
    assertion rather than a clip-exists assertion.** The walk registers one
    `setDirectory()` per directory (the stub wires up both the PROPFIND listing
    and every non-dir entry's GET body from that one call) with each file's
    actual content, its size, its mtime and an etag derived from both — so a GET
    of `lib/kick.wav` returns the committed sawtooth fixture and the dropped
    clip's RENDERED ENERGY is the same closed form `media_browser_drag_local`
    asserts over the LOCAL provider. A directory's size is the documented `-1`,
    matching `SLocalMediaSource`, so the two providers `describe()` one tree
    identically and the two cases can be read against each other. Bounded at 8
    levels / 32 MB per file / 128 MB total, refused loudly rather than silently
    truncated.

43. **An EMPTY `fault=` CLEARS every injected fault**, so one line restores the
    healthy server after a failure phase, and an unknown fault word is refused
    in `readXml` — at PARSE time, never at the first request, because a case
    that misspelled a fault would otherwise assert a healthy server's answer and
    pass while gating nothing.

44. **THE STUB IS NOT A NEXTCLOUD SERVER, and neither case may be read as
    saying otherwise.** Plain HTTP: no TLS (so no certificate validation and no
    TLS-error surfacing), no redirects, no rate limiting, no
    `WWW-Authenticate` challenge, one canonical PROPFIND dialect, not
    Nextcloud's. What is gated is OUR half of the conversation; a real server is
    the manual runbook's job (`docs/MEDIA_BROWSER_MANUAL_GATE.md`).

44a. **`expectAuth=` IS THE ONE THING IT DOES CHECK, and gate 6 added it because
    gate 5c's own PR body named the hole.** Until then the stub never inspected
    the `Authorization` header it was sent, so the credential chain
    `SSecretStore` -> `smedia::CredentialProvider` -> `SWebDavClient`'s header
    -> the wire was EXERCISED end to end and never VERIFIED — those files would
    have been served just as happily for an empty or malformed header, which
    made "the credential path works" an assumption inside a suite built to
    remove assumptions. `SWebDavStub::setExpectedAuthorization()` takes a FULL
    HEADER VALUE, the same spelling `SWebDavClient::setAuthorizationHeader()`
    takes, and answers **401** to anything that is not byte-for-byte equal —
    **ahead of `fault=`**, because a real server rejects a credential before it
    dispatches a method, and **not sticky**, exactly as `fault=` is not: one
    invocation states the server's whole state.

    Three properties of how it is USED, all load-bearing. The expected value is
    a **base64 LITERAL written in the case** (`Basic cXhhdXNlcjpxeGFwYXNz`),
    never derived here from `user=`/`password=` — deriving it would be a second
    implementation of `SMediaAccountManager::basicHeader()` checking itself,
    whereas a literal makes a green browse a statement about the BYTES on the
    wire. `media_webdav_browse` carries a **NEGATIVE CONTROL** (the server is
    told to demand base64(`qxauser:wrong`) while the account still holds
    `qxapass`; the browse must come back as a banner with zero rows), without
    which a stub that silently ignored the setting would pass every row count in
    that file — measured: neutering the compare fails the case at exactly that
    assertion, and nowhere else. And `media_webdav_drop` deliberately does NOT
    set it: one case carrying the credential proof is enough, and a second copy
    of the literal is only a second place to keep in step.

    It remains an exact string compare against one value. It is not a
    `WWW-Authenticate` challenge, not a realm, not a token lifetime, and not
    Nextcloud's app-password semantics.

45. **Both cases are `RUN_SERIAL` and own their keys** — `media/lastSourceId`,
    `media/lastPath/nextcloud:qxastub`, `media/categoryMask`,
    `media/searchRecursive` and everything under `media/nextcloud/qxastub/*` —
    restoring the first four to their `SOpt` defaults and removing the account,
    so `smaragd.ini` comes back with identical CONTENT run to run. **Content,
    not md5**: `QSettings` rewrites the whole file from its own in-memory map
    and does not promise to preserve section ORDER across processes, so an md5
    can legitimately move while every key is exactly as it was. Both set
    `SMARAGD_SECRET_BACKEND=memory` EXPLICITLY rather than inheriting the
    `--test-case` default (§B.8 rule 5, T15): the account only has to work for
    THIS process, and `memory` writes no INI key and no keychain item at all —
    which is why, unlike the gate-5b options cases, neither of these needs
    `dpapi`. `media_webdav_drop` additionally sets `SMARAGD_MEDIA_CACHE_DIR`
    into its own output directory, for the reason `SMARAGD_SIDECAR_DIR` exists,
    and because its "a repeat drop does not re-fetch" claim would otherwise be
    measuring a hit inherited from a previous run.

## The native plugin editor (proposal 33 M6)

46. **`plugin-native-editor` OPENS A REAL `SPluginNativeEditor` AND NOTHING
    REACHES THE SCREEN.** The verb calls `openFor( …, showWindow = false )`, so
    the container's native handle, the attach, the 30 Hz poll timer and every
    action the window commits are the production ones while our own dialog is
    never mapped. That matters more here than for the other widget verbs: a qxa
    run uses the REAL platform plugin — the suite does not set
    `QT_QPA_PLATFORM=offscreen` — so a shown dialog would land on the
    developer's desktop in the middle of the suite.

47. **THE FIXTURE, NOT THE PLATFORM, IS WHAT MAKES THIS GATEABLE.**
    `tw.test.clap.gui` implements `clap.gui` and CREATES NO WINDOW: `create()`
    allocates nothing and `set_parent()` accepts any handle. So the PARAMETER
    FLOW — the part of proposal 33 that can actually be wrong — is exercised
    with no display, while what a real plugin's GUI does inside our container
    stays hand-verification only. The fixture stands in for a user turning a
    knob twice, deliberately by two different routes: `show()` sets Gain to 2.5
    internally and calls `clap_host_params->request_flush` (the only route out
    while nothing renders), `set_size()` queues an edit and requests NO flush
    (so only a `process()` can carry it). A change that services one route and
    not the other passes half of `plugins_test`'s editor section and fails the
    other half.

48. **A CASE THAT OPENS AN EDITOR MUST CLOSE IT.** An editor alive when its
    plugin is torn down is a contract violation both backends report as an
    error, and in a real session it is a window pointing into an unloaded DSO.
    The verb's `close` drains `QEvent::DeferredDelete` before checking, because
    the dialogs are `WA_DeleteOnClose` and `close()` only POSTS the deletion —
    without that drain `expectOpen="0"` would mean "it has been asked to go"
    rather than "it is gone".

49. **`plugin-native-editor action="restore"` DRIVES THE REAL POST-LOAD WALK,
    and its whole point is that it must do nothing.** Proposal 33 D2 re-opens
    every editor a loaded project says was open, and that walk is suppressed in
    a `--test-case` run for a reason stronger than tidiness: a qxa run uses the
    real platform plugin, so an unguarded restore would put plugin windows on
    the developer's desktop in the middle of a suite. `expectOpen="0"` after a
    `restore` is what asserts the guard held — verified by removing it, which
    fails the case at exactly that line.

50. **The `editorOpen` flag is asserted through `assert-plugin-strip`, never
    through a window.** `describeSlot()` appends `|editorOpen=0|1` AFTER
    `|latency=`, for the same reason `latency=` was itself appended last: the
    proposal-08 M5 cases assert contiguous SPANS of that string and a field
    inserted among them would break assertions about something else entirely.

49. **`assert-view-playhead` reads the OPEN VIEW and opens nothing.** It goes
    through `SMainWindow::viewPlayheadFor()` because testkit may not include
    `app/timeline` (inv. 5), the same route `drag-clip-edge` and
    `assert-envelope` take. Reading the view rather than calling
    `splayhead::derivedPos` directly is the point: it gates the WIRING — the
    walk, the resting position and what `paintEvent` actually chose — rather
    than the arithmetic alone. And it opens no tab, because an assertion that
    creates the thing it measures measures nothing; `open-arrangement-tab`
    first.

51. **`close-options-dialog` closes a REAL `SOptionsDialog` through a `QDialog*`
    ON PURPOSE** (AC-b1, 2026-08-21). There is no verb in this repo that can
    click a `QTreeWidget` item or drive a modal `QDialog`'s own event loop, so
    the verb's `page=` attribute stands in for "the user navigated here" (the
    ctor argument) and closing it is what actually exercises
    `SOptionsDialog::done()` — the one place `accept()` and the default
    `reject()` both funnel through, which is what makes `result="ok"` and
    `result="cancel"` equally valid ways to persist `SOpt::OptionsLastPage`.
    `done()` is a PRIVATE override; the call goes through a `QDialog*` because
    a virtual call's access check is against the STATIC type used to make it,
    so the call compiles against the public base and still dispatches to the
    override at runtime. The committed case (`options_last_page.qxa`) only
    ever passes `result="cancel"`: `accept()` ALSO runs `apply()`, which writes
    every OTHER page's widget values back into `SSettings` and, for Audio,
    calls `twSpeaker::setOutputDevice()` — a real device re-open no case in
    this suite has ever exercised through this dialog, and not a risk worth
    taking for a feature that persists identically either way. `assert-media-
    options`' new `initialPage` attribute is the READ-side twin: omitted, it
    builds `SOptionsDialog(nullptr)` with no explicit page (what every case
    before AC-b1 did, now meaning "open on the remembered page" rather than
    always page 0); given, it is what asserts an EXPLICIT page still wins over
    the remembered one. `describeMediaPage()`'s new first line, `page=<name>`,
    is not really a Media-page fact — it is the cheapest way to make either
    side of the round trip assertable without a new describe method.
51. **`drag-note` grew a `modifiers=` attribute (AC-a2)**, spelled exactly as
    `drag-clip-edge`'s own ("ctrl"/"alt"/"shift", "+"-joined) — a separate,
    file-local `parseModifiers()` copy in `seventuitestactions.cpp`, matching
    the existing convention that each gesture-verb file keeps its own rather
    than sharing one (`click-lane`, `select-track`). Threaded all the way
    through `SMainWindow::dragNote` → `SPianoRollView::tkDragNote`, applied to
    every press/move/release the gesture sends, exactly as a real held
    modifier would be.
52. **`wheel-scroll` (AC-g1) sends a REAL `QWheelEvent`**, not a call into the
    factored-out gesture logic directly, so it exercises `SMVActualView::
    wheelEvent()`'s own entry point (the macOS accessibility early-out, the
    `dy==0` guard) rather than skipping past it — going through
    `SMainWindow::wheelScrollArranger()` → `SStdMixerView::wheelScroll()`,
    reusing the `dragClipEdge`/`dragNote` house pattern of a public test entry
    point on the view rather than a re-spelling of the gesture. Its position
    barely matters (only `ZoomHorizontal`'s zoom-to-cursor reads the anchor
    x), so it lands at a fixed interior point; `QApplication::sendEvent`
    delivers it there regardless of visibility, same as every other
    synthesized gesture in this file.
53. **`select-all` (AC-a3) drives the SAME two-step fork real Qt input takes**:
    a `QEvent::ShortcutOverride` offered to `QApplication::focusWidget()`
    first, then either that widget's own `keyPressEvent` or — nothing
    claimed it — `SMainWindow`'s window-level "Select All" `QAction`, through
    `SMainWindow::sendSelectAllShortcut()`. **The known gap**:
    `QApplication::focusWidget()` is always null in a `--test-case` run (the
    main window is never shown), so this verb can only ever reach the
    ARRANGER default branch from a script — see `select_all_scope.qxa`. A
    widget-specific branch (the piano roll's own Ctrl-A) is real, reviewed
    production code that this verb cannot exercise; it needs a shown window.

54. **`double-click-lane` is `click-lane`'s double-click twin, and it exists
    because `double-click-clip` cannot reach a lane with no clip on it.**
    Both go out through the shell (`SMainWindow::doubleClickLane` /
    `SStdMixerView::doubleClickLane`), the same routing reason inv. 5 gives
    `drag-clip-edge`. It sends the real press/release/`MouseButtonDblClick`/
    release sequence `doubleClickClip` already sends, at a `time` rather
    than at a clip's body, so it lands on a clip if one covers that
    position (same resolve as `double-click-clip`) or on bare lane space
    otherwise — the ONLY way a script reaches
    `SMVActualView::mouseDoubleClickEvent`'s bare-folder-lane branch
    (`main/timeline/CONTRACT.md` inv. 30). `double-click-clip` itself grew a
    second real branch the same fix landed: a CONTAINER clip (a registered
    arrangement, a take stack, a plain folder-track window, anything
    `cutIsContainer()` paints blue) now resolves through
    `SMVActualView::tryOpenContainerClip()` instead of the old
    arrangement-only check, so `assert-tab-set` / `assert-lane-view` /
    `assert-lane-overlay`'s `row=` report are what a case reads afterward,
    not a blanket "any other clip is a no-op" (that description is still
    true, but only for a clip that is not blue at all). Gate:
    `qxa.doubleclick_blue_clip_resolve`.

## `assert-take-lane` — the pixel gate on ONE take lane

50. **It classifies on the BODY colour, never on waveform pixels.** A SILENT
    column still paints exactly one waveform pixel, at the midline —
    `drawObjectWaveform` maps `min == max == 0` to `drawLine(x, y, x, y)` — so
    "this column has wave pixels in it" is true of every column of every clip
    and would report each gap as material. `materialCols` / `gapCols` are
    counted from `QColor(160,160,160)` (composited under the inactive-take dim
    when that is what is on screen) and from nothing else.

51. **ACTIVE vs INACTIVE is read off the IMAGE, and the dim is composited
    THROUGH QT.** The verb tries both the lit and the dimmed body colour and
    uses whichever actually appears, reporting `dimmed=`. The dimmed reference
    is produced by doing what `drawTakeLane` does — `fillRect` with
    `QColor(0,0,0,130)` onto a 1x1 `ARGB32_Premultiplied` — rather than by
    reproducing Qt's rounding in the verb, so the two cannot drift. (It happens
    to be exact: 160 -> 78, 26,38,50 -> 13,19,25, 240 -> 118, 10 -> 5.)

52. **The measured clip span is a BOUND, and the case must pin it.** `spanFirst`
    / `spanLast` are the outermost MATERIAL columns, so a clip that BEGINS or
    ENDS in silence reports a shorter span than it occupies, and every
    percentage is taken over that span. `take_lane_domain.qxa` therefore
    asserts the span itself; without that, anything else painted in the body
    colour silently widens the span and drags the percentages off — which is
    exactly what the time grid did (see 53).

53. **RUN IT WITH THE GRID OFF, AND DISABLE THE GRID AFTER `load-project`.** A
    non-emphasised grid line is `QColor(160,160,160)` — the EXACT clip-body
    colour — and is drawn OVER the lanes, so it is a full-height column of
    "material" to any classifier. Measured with the grid on: `spanLast` 500
    instead of 398 and the gap percentages 20/40 instead of 25/50. And a
    `grid-disable` placed BEFORE or immediately after the load does not hold:
    a loaded project comes back with `gridVisible` TRUE whatever the file says
    (its `timelineZoomSecondWidth`, out of the same properties JSON, loads
    correctly), so the disable finds the key already false, no-ops, and the key
    is true again by paint time. That defect is NOT fixed on this branch.

54. **`waveMeanPct` is the only field on the DRAW terminal and may not be
    dropped.** Every gap metric comes from the body fill, i.e. the COLLECT
    terminal. A case that asserts only gaps goes blind to a draw-side
    regression. In `take_lane_domain.qxa` it is also an independent second
    discriminator for the same defect: the correct window reads **62**, the
    broken one **27**.

55. **`assert-take-lane takeRow="-1"` addresses the track's OWN (composite)
    lane.** Same classifier, one different reference colour — the lane fill
    comes from `STrackRendererInline::laneFillColor( *track )`, the function
    the renderer paints with, rather than the take lanes' constant — and no
    dim, because a composite lane is never dimmed. The point is that a
    composite lane and its take lanes become comparable through ONE
    measurement instead of two that could disagree about what "material"
    means. `asset_clip_preview.qxa` uses exactly that to assert the two lanes
    land in the same columns while reading through two different data paths
    (the wrapper's CAPTURE, and each take's own preview).

56. **A PAN assertion must not name a distance — the distance is a USER
    SETTING.** `applyWheel`'s horizontal pan is
    `step = (visibleSpan / 8) * wheelSensitivity_`, and `wheelSensitivity_`
    comes from `SOpt::WheelSensitivityPct` (Edit -> Options -> Mouse,
    persisted as `[mouse] wheelSensitivityPercent`). An absolute
    `assert-lane-view scrollX="N"` therefore encodes the author's own INI and
    passes only on a box left at the 100 % default.

    `follow_scroll_hold` did exactly that and had been RED on the author's
    machine, where the setting is 160 %: it expected 2580 (canvas 430 px at
    secondWidth=1000 -> span 20640 -> span/8) and measured 2580 * 1.6 = 4128,
    to the unit. Not a flake, not a regression, and nothing to do with the
    behaviour under test. It is the same class as the
    `plugin_editor_persistence` INI dependency: a case reading a key a real
    interactive session is free to write.

    Use `snapshot` / `compareTo` (+ `minScrollX` to keep the first check from
    being vacuous), which say what the AC actually claims — moved, then did
    not move, then moved again. **Reading a user-settable key is as much a
    -j hazard as writing one**, and the INI-ownership convention above only
    ever covered the writers.

## 56-57. Extern-file verbs (2026-08-22)

**56. `assert-extern-files` is the ONLY thing that can gate the missing-sample
placeholder.** An unreachable sample used to be dropped together with every clip
on it; it is now kept as a silent placeholder. A dropped clip and a placeholder
are **both SILENT**, so no audio assertion anywhere can separate them — which is
how the drop shipped under a green suite for as long as it did, and why this
verb reads the project's extern-file dictionary directly (`count`, `missing`,
`external`) rather than measuring a render. `count="2" missing="1"` fails in
both directions: a build that drops the file fails the count, one that somehow
loads it for real fails `missing`.

It goes in `<actions>`, never in `<assertions>` — that block dispatches two
hardcoded kinds (`assert-track-count`, `assert-project-matches`) and knows no
verbs at all.

**57. `collect-external-media` runs the production pass**
(`smediadrop::collectExternalMedia`), the same one the resources dock's button
runs, with the confirmation and the report replaced by attribute assertions. It
is NOT undoable, and that is the feature's own contract rather than a testkit
shortcut: the pass copies files.

A case using it must save the project **into `build/test-output/`** before
collecting. Two reasons, and both matter: it is what makes the fixture's own
sample genuinely outside the project folder (a case whose project and sample
share a directory would pass on a build with no self-containment logic at all),
and it keeps the collect from creating files under `tests/`, which nothing in
the suite may do (see "Why `-j` is safe" — `git status tests/` stays clean
across a full run).

## 58-61. fix/editor-ui-and-shortcuts verbs (2026-08-23)

**58. `plugin-native-editor action="open-via-strip"`** is the headless repro
for a real defect, not a convenience wrapper. It builds a THROWAWAY
`SPluginEffectStrip`, PARENTED TO THE REAL MAIN WINDOW (looked up the same way
`sclicklaneaction.cpp`'s `mainWindow_()` does), and opens the editor through it
as `parentForPosition` — the exact call `SPluginEffectStrip::
openParamEditor()` makes — then destroys ONLY the strip, the way
`STrackDetailPanel::rebuildUI()` destroys the real one on every track switch
(`delete pluginStrip_`). `expectOpen="1"` afterwards is the assertion that the
editor's lifetime no longer depends on the strip — see `main/pluginui/
CONTRACT.md` ("The native editor window is never parented to the FX strip")
for the fix and why the plain `action="open"` mode (which always passes
`nullptr`) could never have caught this: it never exercises the
strip-as-parent path at all.

**Parenting the throwaway strip to `nullptr` instead of the main window would
have silently tested the wrong thing, and this was found by doing exactly
that first.** A parentless `QWidget` IS its own `window()` (a top-level
widget's `window()` returns itself), so `SPluginNativeEditor`'s
`parentForPosition->window()` climb (the fix) would land right back on the
strip — indistinguishable, from inside the constructor, from the PRE-fix
`QDialog(parent)` call it replaced. The verb failed with the strip parented to
`nullptr` even on the FIXED binary, which is what caught it: the real
production ownership chain (strip → `STrackDetailPanel` → … → `SMainWindow`)
has to be reproduced far enough that the climb has somewhere durable to land.

**59. `assert-unsaved-changes`** reads `SMainWindow::unsavedChangesForTest()`
— `!QUndoStack::isClean()`, the same predicate `promptSaveUnsavedChanges()`
reads before putting up the "Unsaved work" dialog, but read directly rather
than through the private `hasUnsavedChanges()`: that one ALSO gates on
`currentProject_`, this window's own notion of "a project is open", which a
`--test-case` run never populates (`SActionRunner` puts the project on
`SApplication` directly and never routes it through this window) — found
while writing this case, which failed with "expected unsaved got clean" on
every assertion until the fix. `expect="1"` = changes pending, `expect="0"` =
clean. Exists because `QUndoStack::setClean()` was never called anywhere in
this repository before `main/shell/CONTRACT.md` inv. 51 — this is what makes
that fix headlessly gateable (`save_marks_clean.qxa`) instead of
hand-verify-only.

**60. `deselect-all`** is `select-all`'s twin, driving `SMainWindow::
sendDeselectAllShortcut()` through the identical two-step fork: a
`QEvent::ShortcutOverride` offered to the current focus widget first, falling
through to the window's "Select None" `QAction` only when nothing claims it.
The SAME known gap applies and for the SAME reason: `QApplication::
focusWidget()` is always null in a `--test-case` run (the main window is never
shown), so this verb can only ever exercise the ARRANGER default branch
(`SMainWindow::deselectAllInActiveArranger()`) — `SPianoRollView`'s own
Ctrl-Shift-A handling (`main/eventui/CONTRACT.md` inv. 16) is real,
reviewed code, unreachable from a script the same way its Ctrl-A sibling is.
Gate: `deselect_all_scope.qxa`, mirroring `select_all_scope.qxa` exactly.

**61. `note_resize_selection.qxa`** needed no new verb — `drag-note`'s existing
`edge="end"` mode and `assert-event-editor`'s existing `selection=N` field in
`describe()` were already enough (main/eventui/CONTRACT.md inv. 17). Recorded
here because it is the clearest illustration in this suite of "read what
already exists before adding a verb": the fix it gates (`previewNotes()`'s
`touchedIds` out-parameter) needed a new PARAMETER on an existing internal
function, not a new testkit action.
