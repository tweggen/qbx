# app/timeline — CONTRACT

Purpose: the arrangement UI — SStdMixerView (the timeline canvas: lanes,
clips, drag gestures, playhead), tree view, zoom scrollbar, grid toolbar,
track header/detail widgets, mixer control strip, and SClipPropertiesPanel
(the numeric/textual face of the clip window; the dock lives in the shell).

Public headers: app/timeline/*.h

Depends on (engine): tw/core, tw/graph, tw/devices, tw/playback, tw/sources.
App edges: per tools/check_layering.py (widest view module).

Invariants:
1. Paint paths never block: previews come from page caches with stale
   fallback; locator repaints are driven by the main-thread pump, never by
   audio threads (THREADING.md rule 1).
2. Clip content drawing goes through SObjectRenderer
   (getInlineRenderer()) — do not dynamic_cast concrete object types in the
   canvas (existing casts are debt, not precedent). Where the canvas must
   branch on WHAT a clip is, it asks `SObject::contentKind()`, not what the
   content happens to support: `SCutRendererInline`'s container heuristic
   (`!getRandomSource()`) would classify an event object as a container capture
   and draw a waveform of silence, and `drawTakeLane`'s `dynamic_cast<SCut*>`
   drew an event take as nothing at all. Both are fixed; a new one is a bug.
   The Clip Properties dock follows the same rule with a per-kind PAGE rather
   than a widened struct — the audio page reads SCut-only getters (slip,
   stretch, pitch, formants, warp anchors) that are deliberately not on the
   window interface, so an event clip gets its own page instead of blanks.
3. Gestures COMMIT through actions (e.g. resize-clip on release); live drag
   feedback may use the Raw setters but must leave the model consistent if
   cancelled.
4. All timeline math is frames at project rate; pixel↔frame conversion is
   owned by the view's zoom state.
5. **Lane geometry is per-row, and there is exactly one mapping.** A lane's
   height is `STrackRow::height` — tracks carry individual height scales
   (`setTrackHeightScale`) and one track may own several lanes (take lanes
   and, since proposal 37 P6, automation lanes). NOTHING may compute `row * trackHeight`:
   row→pixel is `SStdMixerView::rowTop()/rowHeight()`, pixel→row is
   `rowAtLaneY()`, and the view-space face of both is
   `SMVActualView::laneTop()/laneHeight()/rowAtViewY()`. The control column
   uses the SAME functions (`controlYOfRow`/`rowAtControlY`) — that identity
   is what keeps the track heads glued to their lanes, and it is asserted by
   lane_alignment.qxa. `getTrackHeight()` is the BASE height (what vertical
   zoom scales), never a given lane's height.
6. **qTrackControlBox_ is a fixed viewport.** Its geometry belongs to the
   holder's layout; the heads inside it carry the vertical scroll and are
   clipped by it. Never move/resize the box — the next layout activation
   silently undoes it, which is exactly how the heads used to come unstuck.
7. A QWidget subclass here must draw its own style-sheet background
   (`WA_StyledBackground` + a `PE_Widget` paintEvent, or a plain paintEvent).
   Declaring a background in a sheet suppresses the palette fill, so a
   subclass without one leaves stale pixels. Scope sheet selectors to the
   class: a bare `QWidget {…}` rule cascades onto every child.
8. Widgets that follow the SELECTION never cache an SLink*: they re-resolve
   SApplication::getCurrentSelectionPaths() on every refresh, because any
   action can destroy a selected link. They refresh off
   SProject::arrangementChanged (selection changes are actions, so they
   already emit it) — there is deliberately no selectionChanged signal.
9. A selection-following widget that also EDITS must survive being refreshed
   by its own commit: re-entrancy flag, blocked signals on every programmatic
   write, commits on editingFinished/clicked only (never valueChanged), and
   no setFocus() from the refresh path. See SClipPropertiesPanel.
10. Level meters (proposal 34) read FROZEN PAGES through
   twComponent::getPageIfExists and never block, wait or demand: a miss decays
   the meter, it does not stall it. The tap is the track's ROOT component (its
   twRewire) — post-fader, post-FX, and the only per-track component that
   caches pages (twTrackMix allocates a fresh page per call; twPluginChain
   forwards). Meters follow mute/solo by an EXPLICIT model check as well as the
   emergent nulled-plug behaviour, so the result is order-independent. That
   check is `ssolo::isLaneAudible` (app/model/ssolorules.h) and MUST NOT be
   re-spelled here: the rule is whole-tree (a soloed lane keeps its ancestor
   folders audible, a soloed folder keeps its subtree audible), and the two
   meter call sites' local copies of the old direct-children-only rule are how
   the meter and the ear could disagree about a nested lane.
11. A meter must not repaint unless what it would DRAW changed: pixel-quantized
   comparison against what was last PAINTED (not last computed), sub-rect
   update(), and zero work at all when hidden. 30 heads x 30 Hz is otherwise a
   repaint storm, and a 20 dB/s decay moves a 48 px bar by well under a pixel
   per tick. The sub-rect path is kept for the ONE-LANE case specifically —
   which since proposal 36 B8 means a MONO project only, because a head shows
   `min(width, MONITOR_LANES)` lanes and a stereo project therefore draws two
   (inv. 11a). The clause read "every head on a mono or stereo project" until
   B9. A multi-lane meter still tests before
   repainting, but repaints the widget, because the lanes move on both axes and
   a union of per-lane rects would buy nothing on the handful that exist.

11a. **How many lanes a meter shows is the MOUNT's decision** (proposal 36 B8),
   and `SLevelMeter` implements both answers rather than choosing:
   - The **track head** and the **transport master meter** show
     `min(width, SLevelMeter::MONITOR_LANES)` = at most TWO, dividing a fixed
     8 px `BAR_THICKNESS` among them. This follows the device rule twSpeaker
     states (`L = ch0; R = (width >= 2) ? ch1 : ch0`, the rest computed in full
     and dropped at the device): a 120 px control column has ~13 px of slack
     (proposal 34's measurement), six lanes in it would be 1 px each, and
     capping at the pair you can actually HEAR is the one reduction the product
     already commits to.
   - The **Track Detail dock** shows EVERY channel and grows its short axis to
     do it (`setGrowWithLanes`). It is the answer to "where do I see channel 4".
   The cap is ANNOUNCED, never silent: `describe()` reports `lanes` and `width`
   as separate fields, and the tooltip names the project's width and points at
   the dock. A user seeing two bars on a six-channel project can find out why.
   The width comes from the TAP's `getOutputChannels()`, not from `SProject`, so
   the meter and the audio read one number — the same number §4.5's
   width-mismatch rule compares a cached page against.
12. **A track-level gesture acts on the SELECTION when it is aimed into it,
   and on the clicked track alone otherwise.** That is the single rule behind
   multi-track mute/solo/arm, the structural operations (remove / indent /
   outdent / group / ungroup / lane height / take lanes) and the head drag;
   `SStdMixerView::selectionTargets()` is its ONE implementation and nothing
   may re-spell it. Aiming outside the selection must never reach a lane the
   pointer is not on. Consequences that are easy to get wrong:
   - The set lives on the MODEL (`SStdMixer`, QPointers so a removed track
     cannot dangle) with one distinguished PRIMARY — the lane the Track Detail
     dock follows. The GESTURES (which modifier does what, the Shift range
     anchor) live here, in the view.
   - Structural operations run over `pruneNestedTargets()` (outermost tracks
     only: a folder carries its subtree) and re-resolve every path BETWEEN
     steps, because each applied action shifts the indices the next one would
     have used. Order matters and is opposite per operation: remove and outdent
     go bottom-up, indent and drag-insert go top-down.
   - A broadcast is ONE undo step (a QUndoStack macro) — the user made one
     gesture. Each target gets the ABSOLUTE value the pressed button now shows,
     so a mixed selection ends up uniform rather than inverted lane by lane.
   - A press on a head's GRIP does not collapse a selection it is part of;
     that is what makes dragging several tracks possible. A press anywhere
     else on the head applies the click semantics.
13. There is ONE fader curve, app/timeline/sfadercurve.h. Both faders onto a
   track's volume (the arranger head and the Track Detail dock) use it; the
   dock previously did a naive value = dB*10 and disagreed with the arranger
   about where a given dB sits.

14. **The track head's second button pair is density-gated, and the gate is
   a FIT test, not a constant** (proposal 37 P4, design 6.1). "A" (automation
   mode) is Full-density only AND only while a six-button column still fits
   the lane; "I" (instrument) additionally requires slot 0 to BE an instrument
   (`SPluginSlot::getDescriptor().isInstrument` — the DESCRIPTOR, so a slot
   whose plugin is missing on this machine keeps its identity). Five 20 px
   buttons need 108 px, six need 130, and Full starts at 132 — so an
   unconditional sixth button clips exactly the shortest Full lanes. The seam
   is `SSMVMixerControl::describeHead()`, a `describeMeter()` sibling that
   re-applies the density rules for the current size first (Qt delivers no
   resizeEvent to a widget that was never shown) and reports `fitW`/`fitH`,
   which is "hiding beats clipping" made assertable.
15. **The snap spec's grid DIVISION reads the tempo map** (proposal 37 P4).
   `SSnapSpec::setGridDivision("1/16")` is parsed by
   `SQuantizeNotesAction::gridTicks()` — the ONE parser, shared with
   `quantize-notes` and the event editor — and converted through
   `twTempoMap`, the single tempo authority (D2), never by multiplying
   60/bpm. An EMPTY division is the pre-36 beat snap byte for byte, which is
   what keeps every committed case's snapped positions unchanged.
16. `SMVActualView::secondWidthChanged` is now EMITTED (it was declared and
   never fired — a FIXME in `setSecondWidth`) and carries a `double`. The
   event editor's time axis mirrors this widget's px↔frame mapping, so an
   `int` would quantise every zoom below 1 px/s. `contentView()` and
   `snapSpec()` are exposed for the SHELL, which is the only module that sees
   both app/timeline and app/eventui; the editor must not depend on the
   arranger.

17. **Automation lanes are STrackRow SUB-LANES, and the whole feature lives in
   `timeline/src/sautomationlane.{h,cpp}`** (proposal 37 P6, design 6.1).
   `STrackRow::subKind` is `{None, Take, Automation}` and `isSubLane()` reads
   it — so an automation row gets inv. 5's geometry, the track's single head
   spanning its whole lane group, and `assert-lane-alignment` for free. It also
   carries the lane's identity: `autoTarget` (the ParamRef spelling) plus
   `autoSlotIndex` for a `param:` lane, which is exactly the address every
   automation verb takes.
   - The painting, the hit test, the gesture state machine, the picker menu,
     the clip-envelope hit test and the testkit driver are ALL in that new
     file, and so are the definitions of the five `SStdMixerView` members that
     are automation code. `sstdmixerview.cpp` keeps only the call sites — it
     grew by 36 lines for the whole feature, against a budget of 100, and it is
     already the largest file in the app (see Known debt).
   - The curve is sampled PER PIXEL through `SAutomationLane::valueAt`, the
     same call `assert-automation-value` makes. Step / Linear / Exp therefore
     come out right by construction; a per-segment painter would be a second
     implementation of the interpolation and could disagree with the ear.
   - Each target draws its OWN domain (`sAutoScaleFor`): dB through THE fader
     curve for `self:Volume` (inv. 13 — anything else would put a given dB at
     different heights on the lane and on the fader), 0/1 stepped for
     `self:Muted`, the plugin's DECLARED range for `param:<id>` (asked of
     `SPluginSlot::paramRows()`, because timeline may not include tw/plugins),
     a linear factor over [0,1] for `cut:Gain`. A shared 0..1 scale would draw
     a −60 dB fade as a flat line on the floor.
   - Gestures obey inv. 3 with an explicit REVERT-THEN-ACT: the live drag
     mutates the point table directly for feedback (no engine push, no undo
     entry), and the release puts the pre-drag table back BEFORE submitting the
     verb — otherwise the action would find nothing to change, its undo step
     would be a no-op and a redo would double-apply. Click on empty space =
     `add-automation-point`, drag = one `move-automation-point`, primary-click
     on a point = `remove-automation-point`, Alt-drag = tension through
     `set-automation-points` over that one frame, Shift-drag = marquee, Delete
     = one `set-automation-points` over the marquee's span. A press this file
     claims swallows the WHOLE press/move/release triple (`consumed_`), or the
     move would fall through to the clip gestures on a lane that has no clips.
   - `cut:Gain` is drawn as an OVERLAY on the clip by the cut renderer (after
     `drawWarpMarkers`), never as a sub-lane, because the curve lives on the
     WINDOW and travels with it across placements and takes. Its gestures are
     ARMED (`setClipEnvelopeEdit`, OFF by default) — that is what keeps every
     clip-body gesture (move, slip, duplicate, stretch) exactly as it was.

18. **There is ONE pruning walk for EVERY per-track UI-state set**
   (`SStdMixerView::pruneUiState`, proposal 30 §E.5, proposal 37 P6). The
   take-lane set, the per-track height scales and the shown-automation set are
   all keyed by `STrack*`, and a removed track otherwise leaves a dangling key
   for a later track allocated at the same address to inherit — a new lane
   that mysteriously remembers a deleted one's state. The walk is over the
   MODEL, not over `rows_`: a collapsed folder's children are alive and have
   no row, and pruning against the rows would forget their state on every
   fold. It runs from `rebuildRows()`, which is the one funnel every
   structural change already passes through.

   The FOLD set used to be pruned here too, and isn't any more
   (fix/track-list-polish m): fold state moved onto `STrack` itself
   (`STrack::isCollapsed()`/`setCollapsed()`, a plain serialized attribute,
   written only when true) so it can be saved with the project. Being an
   ordinary object attribute it dies with the object automatically — no
   dangling key is possible — and it gets a save/load round trip for free
   through the path every other track attribute already takes.

19. **The head's "A" button governs EVERY automation lane the track owns** —
   its own `self:` lanes and its plugin slots' `param:` lanes alike, in one
   undo macro of `set-automation-mode` actions (proposal 37 P6). That is the
   only reading under which a single button on the head is not ambiguous the
   moment a track owns two lanes, and it matches what a per-track automation
   mode means in a reference DAW. A left click cycles Off → Trim → Read →
   Touch → Latch → Write; a right click picks. A track that owns NO lane gets a
   `self:Volume` lane created in the mode being cycled to, so the button is
   never a silent no-op. The button KEEPS the letter A at every density (three
   of the six modes start with a letter another 20 px square in the same column
   already uses); the mode is carried by colour and tooltip on screen and by
   `Amode=` in `describeHead()`, which is reported at EVERY density because the
   button hides on a short lane while the mode does not stop existing.

20. **Touch/Latch/Write are UI RECORDERS and commit ONE action per gesture**
   (`SAutomationRecorder`, app/shell, proposal 37 P6 / design D5). The engine
   cannot tell them from Read; what differs is entirely on this side. The
   arranger's fader and the plugin parameter editor's slider both feed the ONE
   app-wide recorder — which is why it lives in the shell and not in a view,
   since app/timeline and app/pluginui cannot see each other. A control write
   during a pass must NOT submit its ordinary verb: `applyVolume_` and
   `onParamSliderChanged` hand the value to the recorder and return, and the
   pass lands as a single `set-automation-points` when it ends. An action per
   block would put thirty entries a second on the undo stack.
   - While a Read-FAMILY lane exists, the control DISPLAYS the curve's value at
     the position being heard, pumped from `SApplication::meterTick` — the one
     main-thread tick that keeps running at a static position and for a tail
     after the transport stops (proposal 34). A control being RECORDED is
     exempt: it must show the hand, not the curve, or the fader fights the
     finger dragging it.
   - Plugin GESTURE punch-in (`ParamGestureBegin/End` out of the plugin) is NOT
     wired: those events reach the host only inside `process()`, on a worker
     thread, at freeze time, and there is no native plugin editor to raise one
     (proposal 33 M3). The app's own slider press/release is the punch-in.

How to test: lane_alignment.qxa (lane geometry + head placement under zoom,
scroll, per-track heights and take lanes), track_list_scroll_padding.qxa
(fix/track-list-polish l — the vertical scroll-range padding, via
assert-scroll-range's numeric gate: scrolling to the scrollbar's own
maximum() brings the true last row fully into view when content overflows
the viewport, and adds no scroll room when it already fits),
track_list_view_roundtrip.qxa (fix/track-list-polish m — fold state and
zoom/pan survive a save/load round trip, via assert-lane-view),
test_track_column_expansion.qxa,
test_track_width_dragging.qxa, clip_properties_actions.qxa (the property
verbs the panel submits), screenshot actions in the render cases;
meter_levels.qxa + meter_postfader.qxa (the meter's levels, its miss path and
its density rules, via the real head built off screen);
track_head_density.qxa (the head's density rules and the instrument /
automation buttons, through the real head built off screen);
multitrack_selection.qxa (the click semantics, the mute broadcast and its
single undo step, the multi-track head drag and Group over a selection — all
through the real widgets via select-track / track-head-toggle / drag-track);
automation_lane_gestures.qxa (add / move / delete / tension / marquee and the
clip-envelope overlay, all through the REAL mouse handlers via
drag-automation-point, each gesture one undo step, plus head/lane identity
with two automation lanes AND a take lane on one track and a PNG of the
canvas), automation_write_pass.qxa (a Touch pass over a real transport: three
ticks, ONE undo step, and the curve heard through the capture backend) and
automation_head_mode.qxa (the mode glyph at three densities and a PNG of the
head).

Known debt: sstdmixerview is the largest file in the app and knows every
object type; per-object renderer extraction (proposal 14 slices) is the
long-term shape.

21. **The ARM button is a VERB and its right-click menu is the INPUT SELECTOR**
    (proposal 21 L1b, design D9). Arm used to be a direct model write on the
    grounds that it is transport state; it is `arm-track` now because it decides
    whether a track is MONITORED — which re-wires the mixer and hands a plugin
    chain to another thread — so it must pass through the one place that owns
    that ordering, and because a user who armed the wrong lane of a
    multi-selection has the same right to undo it as one who muted it. The menu
    carries Monitor (auto/on/off), Input device and the channel mask, all over
    the SELECTION like every other head toggle, all through their verbs. The
    device and the mask are two halves of ONE portable string, and the legacy
    `recordingChannels` mask is kept in step with it so the recording path and
    the monitoring path cannot disagree about which channels this track hears.
    The tooltip ANNOUNCES the live state, including the one failure a user
    cannot otherwise see: `openLive()` refuses a device whose rate is not the
    project rate.

22. **A FOLDER LANE'S BACKGROUND IS A SUM OF ENVELOPES — never a freeze, never
    blocking, and never scaled by the lane's own fader** (proposal 39 M3).
    `STrackRendererInline::draw()` paints
    `STrack::collectChildSumEnvelope()`'s probes on the lane background, after
    the fill and before the clip loop, so the overlay sits above the background
    and behind the folder's own clips. Three things about it are load-bearing:

    - **The exact answer exists and must not be used here.**
      `folderTrack->getPreview()` already returns the folder's real summed
      output, through `SObject::straightCalcPreviewData()`'s container branch —
      and that branch reaches its pages via `requestPage()`, which DEMANDS A
      FREEZE. Calling it from `paintEvent` renders the folder on the UI thread,
      which inv. 1 forbids. The overlay is therefore built from previews that
      already exist: it over-states where children are out of phase, and it
      cannot see child plugins, instruments or automation, which live only in
      frozen pages. It is a hint about where material is, not a meter.
    - **The lane's own fader, mute and inserts are nowhere in it.** That is
      M2's rule ("a drawn waveform describes the audio its object PRODUCES; the
      lane it is drawn on never scales it"), and this overlay IS the lane. A
      child one level down is not the lane being drawn on, so a child's own
      fader does scale its contribution.
    - **The colour is DERIVED from the lane's final colour**
      (`STrackRendererInline::laneFillColor()` lightened, at partial alpha),
      never a fourth hardcoded constant, so it follows selection and every
      `STrackColorModifier` state. What is contractual is the RELATION, and it
      is measured: strictly lighter in luminance than the lane fill, strictly
      darker than the clip body `QColor(160,160,160)`. Gate:
      `assert-lane-overlay` in `folder_sum_preview.qxa` — the first thing in
      this repo that gates the arranger CANVAS's pixels at all.

    Cost: (visible clips in the subtree) x (lane width in pixels) probe
    lookups per repaint, each an index into an array a child's preview already
    built — about what the same clips cost on their own lanes when the folder
    is expanded. No cache, no snapshot, no invalidation, no thread.

    The collapsed folder is the whole point of it, and it is gated since
    proposal 39 M3a: `collapse-track` drives `toggleTrackCollapsed()` — the
    fold triangle's own call — so a case can shut the folder, watch every lane
    below it move up two rows, and find the same overlay, at the same pixel
    count, on the folder's row. The paint is identical either way by
    construction (the folder's row is drawn by this renderer whether or not its
    children have rows); what the gate closes is the CLAIM, not a suspected bug.

23. **A take-lane click SELECTS THE STACK, and Alt on the body slips the TAKE
    UNDER THE POINTER, not the active one.** A plain click still submits
    `SSelectTakeAction` (the comping gesture, proposal 17 phase 3) — but it
    ALSO calls the same `submitSetSelectionAction`/`submitToggleSelectionAction`
    the composite lane's own click does, on `lastClickSLink_` (the take
    STACK's own outer link — a stack is one clip on the timeline; only WHICH
    take sounds is per-lane, so there is no such thing as "select take row 2"
    in the general selection). Skipping this left whatever was selected
    before the click (or nothing) as the selection, so a keyboard command
    aimed at "the clip I just clicked" — split (`s`) being the one that
    surfaced it — silently acted on a stale, unrelated target instead.
    `SSplitClipAction` was ALREADY stack-aware (it splits every take when
    handed the stack's own path); only the click was failing to point at it.
    Alt-drag on a take-lane clip body arms a slip exactly like the composite
    lane's, with one difference: `clipDragTakeIndex_` (-1 outside a take
    lane) records WHICH take was under the pointer, so the live drag and the
    release's `SResizeClipAction` (its `take` parameter existed since phase 4
    for edit-group broadcast, and needed no change) edit that take's `SCut`
    while `lastClickSLink_` stays the stack's link throughout — for position,
    path and repaint-rect purposes only. `lastClickSLink_` must NEVER be
    fed to `ensureSCut()` while `clipDragTakeIndex_ >= 0`: it wraps an
    `STakeStack`, not (yet) an `SCut`, and `ensureSCut` treats "not an SCut"
    as "wrap it in a new one" — which would silently replace the stack's own
    link with a bogus `SCut(project, *stack)` and orphan every take in it.
    Move/stretch/loop gestures are still unclaimed on a take-lane row.

24. **A double-click on an EVENT clip opens the event editor for it**
    (`SMVActualView::mouseDoubleClickEvent`), through `SMainWindow::
    showEventEditor()` — never through `app/eventui` directly, which this
    module may not depend on (same reason `SEventTimeAxis` linking lives in
    the shell). It relies on Qt's OWN double-click delivery order: press /
    release / `MouseButtonDblClick` / release — there is no SECOND
    `QMouseEvent::MouseButtonPress` for the second click, so the leading
    press (which already ran the ordinary single-click selection handler)
    is what `lastClickSLink_`/`lastClickTrack_` hold by the time
    `mouseDoubleClickEvent` checks `contentKind()`. `tryAddMarkerAt()`
    re-resolves both at the exact double-click position as its own first
    step, which is why this check does not call `updateLastClickVars()`
    again — doing so a second time would just repeat what that call
    already did. Any OTHER clip is a no-op (matches design: only an event
    clip has an editor to open). Test entry point: `double-click-clip`
    (`SStdMixerView::doubleClickClip`, `drag-clip-edge`'s twin).

## The `media:` drop branch (proposal 38 gate 3)

23. **`SMVActualView::dropEvent` has ONE new branch and it is five lines:**
parse the `media:` payload into an `SMediaRef` and call
`smediadrop::placeWhenLocal`. Everything under it belongs to `app/media`. Two
things are contractual:

- **The `file:` and `asset:` branches are byte-unchanged.** That is what keeps
  every existing placement path — Insert sample, an OS file drop, the resources
  dock, and gate 2's LOCAL browser rows, which still emit `file:` — exactly as
  it was.
- **The branch hands over the `STrack`, not `trackPath`.** A pending placement
  must hold the target's IDENTITY, because an index-path is a position in a tree
  the user is free to edit during a 40 MB download, and a clip landing silently
  on the wrong lane is worse than not landing at all (proposal 38 §B.5, trap
  T18). The arranger still derives `trackPath` above, for the two branches that
  place synchronously and for which it cannot go stale.

The arranger never learns what a source, a fetch or a cache is. The payload is
the whole interface, exactly as it already was for `file:` and `asset:`.
