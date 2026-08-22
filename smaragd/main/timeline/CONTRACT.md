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
   Qt's own clipping only handles a head whose top is negative; a head
   straddling `[0, SMV_TIME_RULER_HEIGHT)` needs the explicit clamp inv. 29
   describes.
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
scroll, per-track heights and take lanes — including inv. 29's ruler-band
clamp on whichever row a `set-lane-view topRow=` lands the scroll offset
inside, now that a requested row can be pixel-clamped short of its own top),
track_list_scroll_padding.qxa (originally fix/track-list-polish l's
row-granular padding hack; since fix/arranger-ui-fixes C the SAME numeric
gate — `assert-scroll-range`, scrolling to the scrollbar's own `maximum()`
— passes for a different reason: pixel granularity reaches the true content
bottom directly, so `maxScroll=` is now PIXELS and no longer needs the old
"+1 row" correction; the case's own numbers did not need retuning, only its
header prose describing the mechanism),
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

    **Every place this invariant's gestures ask "does this link carry take
    lanes" goes through `takeStackOfLink( SLink* )`, never a bare
    `dynamic_cast<STakeStack*>( &lk->getSObject() )`.** Found alongside inv.
    30: proposal 17's modern shape places a take column directly (`SLink ->
    STakeStack`), but a real saved project can also carry an OLDER shape,
    `SLink -> SCut` whose CONTENT is the `STakeStack` (loadable, not
    buildable through any verb in this suite — `add-take` always produces
    the direct shape). The bare cast matches only the modern shape, so on
    the legacy one `maxTakesOf()` counted zero takes (no row was ever
    built), `drawTakeLane()` had nothing to paint even if a row existed, and
    the take-lane press/drag handlers (the ones this invariant describes)
    could not resolve a `stack` either — showing take lanes for such a clip
    (inv. 30's rule 2) did nothing OBSERVABLE, which is exactly the
    "double-click does nothing" symptom one layer deeper. `takeStackOfLink()`
    resolves both shapes; it is deliberately scoped to take-lane
    RENDERING/INTERACTION only — the many other `dynamic_cast<STakeStack*>`
    sites in `objects/cut`'s actions (add-take, remove-take, resize-clip,
    select-take, set-clip-name/pan/volume, set-formant-*, set-pitch,
    split-clip) and in `ctPitchNudge`/`SClipPropertiesPanel` ask a different,
    per-action question (what does EDITING this clip mean on the legacy
    shape) that this resolve does not answer for them and that stays open.
    **The drawn/dragged EXTENT comes from the LINK's own object's duration
    (`lk->getSObject().getDuration()`), never the inner stack's
    (`stack->getDuration()`)**: on the legacy shape `lk->getSObject()` is
    the WRAPPING cut, whose own window governs the clip's displayed extent
    independent of the stack's raw material (it may be slipped/trimmed);
    on the modern shape `lk->getSObject()` IS the stack, so this is
    identical to what was there before and no golden or existing case moved.
    Gate: `qxa.takestack_legacy_wrap_lanes` (a hand-written `.qxp` fixture,
    `tests/legacy_takestack_wrap.qxp`, since no verb builds the shape),
    proven failing on the pre-fix binary before being fixed.

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
    already did. A CONTAINER clip (anything `scutrndrinline.cpp`'s
    `cutIsContainer()` paints blue) is resolved by inv. 30 below, checked
    BEFORE this branch; any other clip is a genuine no-op (matches design:
    a plain audio clip has no editor to open). Test entry point:
    `double-click-clip` (`SStdMixerView::doubleClickClip`, `drag-clip-edge`'s
    twin).

25. **The Feel Flow compliance heatmap is a bottom band, drawn AFTER the clip
    loop, read-only and never a demand** (proposal 40 M2,
    `STrackRendererInline::drawFeelFlowBand`). It is deliberately NOT in the
    slot proposal 39's folder-sum overlay uses (right after the lane fill,
    before the clips): that slot is BEHIND every clip body and would paint
    the band invisible on exactly the clip-covered lanes this feature
    targets. A missing OR stale track analysis (`STrack::feelFlowStale()`)
    paints NOTHING — checked with two cheap reads per repaint (`feelFlowStale()`,
    the atomically-cached `STrack::feelFlowForUi()`), never a block, never a
    demand (inv. 1), mirroring `SPlainWave::onsetsForUi()`'s discipline: the
    one bounded sidecar read a cache MISS may do happens on the FIRST paint
    after a fresh result only. Timeline frame -> "groove.res" hop index is
    PLACEMENT ARITHMETIC ONLY (one integer divide by `hopFrames`) because the
    M1b bounce is a whole-project render anchored at project frame 0 — no
    warp map, no clip-relative offset, anywhere in the paint path.

    **The colour law changed 2026-08-21** (requester follow-up: "rainbow, in
    case I miss shades of grey" — the original partial-alpha tint mixed from
    the lane fill was too subtle). The band now paints fully OPAQUE from ONE
    authoritative LUT, `STrackRendererInline::feelFlowPalette()`: a quantized
    24-step hue ramp, red (low compliance) through yellow to green (high
    compliance) — traffic-light semantics, red is the spot to edit/listen/
    overdub. The pixel gate got STRONGER, not weaker, to match: exact-RGB LUT
    membership (`SMainWindow::describeLaneOverlay`'s band-mode classification,
    `assert-lane-overlay`'s `minLutPixels=`/`maxLutPixels=`/`minLutSpread=`)
    replaces the old luminance-relation floor as the load-bearing check, and
    the negative ("nothing painted") cases now assert an EXACT zero LUT
    pixels rather than a measured noise floor with margin — opaque paint
    makes an exact match sound, where the old partial-alpha tint could not
    honestly reach a literal zero over a clip's own waveform paint. See
    `main/objects/track/CONTRACT.md`'s Feel Flow section for the LUT itself.

26. **The Feel Flow Track Detail panel section is read-only and pumped from
    `SApplication::meterTick`, never a second implementation of the overlay's
    own read path** (proposal 40 M3, `main/timeline/include/app/timeline/
    sfeelflowpanel.h`). Its readouts (compliance at the playhead, per-
    pendulum energy bars, the §3.5 lean/drive pair) go through the SAME
    `STrack::feelFlowForUi()` cached read inv. 25 already gates, indexed by
    `locator / hopFrames` exactly as the overlay indexes it — never a
    separate store read, never a demand, never a block. No control on the
    panel bumps a content epoch: the Analyze button calls the SAME
    `STrack::startFeelFlowBounce()` the `feel-flow-analyze` verb calls
    (background, non-undoable — scheduling analysis is not an edit to the
    arrangement), and the mode combo / Learn button submit ordinary
    undoable actions (`set-feel-flow-mode`, `learn-feel-flow`,
    `main/objects/track/CONTRACT.md` inv. 26-28) that touch model state
    only. `describe()` is the SAME function the on-screen labels are built
    from, callable synchronously with no running tick (`SMainWindow::
    describeFeelFlow`/`grabFeelFlow`, the `assert-track-head` shape — a
    single throwaway widget, never a whole dialog page) — so a testkit
    assertion and what a user actually sees can never independently drift.

27. **Vertical scroll is PIXEL-granular, and `SMVActualView::upperLeftY_` is
    the AUTHORITY** (fix/arranger-ui-fixes C, replacing the row-granular
    scroll invariant 5 predates). `setTopPixel(int y)` is the only writer,
    clamped to `[0, max(0, totalRowsHeight() - (height() -
    SMV_TIME_RULER_HEIGHT))]` — the same bound `SStdMixerView::
    verticalScrollMaximum()` computes for the real scrollbar, so neither can
    show blank space below the content. `topRow_` (`getTopRow()`) is a
    DERIVED convenience — `rowAtLaneY(upperLeftY_)` — kept because some
    callers only need "which row is on top", never written directly.
    `setTopOffset(idx_t row)` is a thin wrapper, `setTopPixel(rowTop(row))`,
    kept for the testkit (`tkSetTopRow`) and for the re-anchor call sites
    (`setTrackHeightScale`, `onArrangementChangedRows`, `refreshTrackTree`,
    `SMVActualView::setTrackHeight`) — every one of which re-anchors by
    FRACTION of the (just-changed) `totalRowsHeight()`
    (`SStdMixerView::reanchorScrollByFraction()`), snapped to a row
    boundary, not by the old row INDEX: a row's own height can change size
    while it holds the anchor, and a structural change can renumber every
    row. `qScrollVert_`'s value/pageStep/maximum are pixels too
    (`SStdMixerView::recalcPageStep()`/`verticalScrollMaximum()`); the old
    "+1 row of scroll headroom" hack (track_list_scroll_padding.qxa) is
    GONE — it existed only because row granularity could not reach a
    partially visible last row, and a pixel offset reaches the true content
    bottom directly. A wheel notch (`SMVActualView::applyWheel()`,
    `SOpt::ScrollVertical`) moves a fraction of the CURRENT base lane height
    (`SMV_WHEEL_VSCROLL_FRACTION`), scaled by `SOpt::WheelSensitivityPct`,
    with no accumulator — unlike the old lane-quantised step, any sub-notch
    delta already maps to a valid pixel amount.

28. **The horizontal scrollbar's domain is project FRAMES, not a fixed
    `HSliderRange`-step index** (fix/arranger-ui-fixes B — `HSliderRange`
    is gone). `qScrollHoriz_->value()` IS `SMVActualView::getLeftOffset()`;
    `SStdMixerView::timeSliderMoved()`/`avLeftOffsetChanged()` no longer
    rescale through `dur`, so a drag or a wheel notch lands EXACTLY where
    requested, at any zoom or duration — this closed the sub-step dead zone
    a wheel notch used to fall into when zoomed in, and the out-of-range
    `double`→`int` conversion `dur <= 1` (an empty/near-empty arrangement)
    used to trigger. The bar's range comes from
    `SStdMixerView::horizontalExtent()`: at least the content duration, at
    least far enough to cover the current pan plus one visible span (so an
    UNBOUNDED wheel-pan past the last clip — `applyWheel()`'s
    `SOpt::ScrollHorizontal` case never clamps the pan itself — always has
    a scrollbar that can already reach it, since `avLeftOffsetChanged()`
    recomputes the extent before syncing the bar's value), and never below
    a ten-second floor. `avLeftOffsetChanged()` wraps its `setValue()` in a
    `QSignalBlocker`, so the wheel's exact frame offset is never
    round-tripped back through `timeSliderMoved()`. `SMainWindow::
    arrangerSetZoomPan()` is unaffected — it already spoke frames.

29. **The lane paint is CLIPPED below the ruler band, and the head column
    has a matching clamp** (fix/arranger-ui-fixes C, a consequence of inv.
    27's pixel granularity). `SMVActualView::paintEvent()` wraps the lane
    loop and the record overlay in `p.setClipRect(0, SMV_TIME_RULER_HEIGHT,
    w, h - SMV_TIME_RULER_HEIGHT)`: with a partially scrolled top row,
    `laneTop(firstVisibleRow)` can be less than `SMV_TIME_RULER_HEIGHT`
    (even negative), and nothing previously stopped a lane drawing OVER the
    ruler. `SStdMixerView::layoutControlColumn()` applies the mirror fix to
    the track-head column, but ONLY to the ONE row whose lane GROUP
    contains the scroll offset (`rulerStraddleHeadRow()`) — every row
    strictly before it already ends at or before the ruler line by
    construction (adjacent prefix-sum geometry), and clamping it too would
    be wrong, not merely redundant. `tkCheckLaneAlignment()` applies the
    SAME clamp (the shared static `clampHeadToRulerBand()`) when computing
    its expected geometry, so the checker and the real head cannot silently
    diverge — the two must stay ONE definition, as inv. 5 already requires
    of row→pixel geometry generally.

30. **A double-click on a CONTAINER clip never falls through to nothing**
    (fix/arranger-ui-fixes, the "blueish clip does nothing" bug). Every
    content object `scutrndrinline.cpp`'s `cutIsContainer()` paints BLUE —
    a registered arrangement, a take stack, a plain folder-track window, a
    nested `SCut`, or anything else with no random source — is resolved by
    `SMVActualView::tryOpenContainerClip()`, checked in
    `mouseDoubleClickEvent` right after inv. 24's marker-strip branch and
    BEFORE the event-clip check (inv. 24): a registered arrangement opens
    or fronts its tab (`arrangementNameOf()`, unchanged from before this
    invariant existed); a take stack shows THAT CLIP'S TRACK's take lanes
    (`SStdMixerView::toggleTrackTakesExpanded`) — SHOWN, never merely
    toggled, so a second double-click cannot close lanes the first one just
    opened; a plain track with children reveals that track's own lanes,
    expanding every collapsed ancestor between the model root and it; and
    anything else that is still blue tries the tab route and, failing
    that, REPORTS the dead end (status bar + `TW_LOGW("ui.timeline", …)`
    naming the content's class and kind) rather than doing nothing.
    A take column is unwrapped FIRST — `SObject::windowTakeAt(-1)` on the
    clicked link's object, before the `dynamic_cast<SCut*>` — so a
    container cut sitting inside a take column (`SLink -> STakeStack`
    directly, what `add-take` builds at runtime) resolves exactly as the
    same cut would on its own; skipping the unwrap is how this dead-ended
    silently before (the cast on the STACK itself always failed). A BARE
    folder lane — no clip of the track's own under the pointer, a real row
    whose track has child tracks — toggles that folder's fold the same way
    the head's fold triangle does; `double-click-clip` cannot reach this
    (it needs an existing clip to address), so `double-click-lane` is the
    test entry point for it (`SStdMixerView::doubleClickLane`,
    `click-lane`'s double-click twin). None of this is undoable itself:
    opening a tab, expanding a fold or showing take lanes is view/UI state,
    same as inv. 24's editor-opening. Gate: `qxa.doubleclick_blue_clip_resolve`
    plus the pre-existing `qxa.tabs_doubleclick_drillin` for the unchanged
    arrangement-tab half.

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

### inv. 24 — the playhead a view draws is ITS OWN root's (proposal 09 §15)

`SMVActualView` draws `localPlayhead()`, never
`SApplication::getGlobalLocatorPos()`. The transport's position is a **master
frame**; in an arrangement tab it names nothing, and drawing it there was a
green line borrowed from another coordinate system.

The master view returns the locator unchanged and pays for nothing else, which
is what makes the rest of the suite the gate for "the master did not move".

Three consequences a change here must keep:

- **`localPlayhead()` is called from `paintEvent` and must never block or
  render.** It walks the object tree and asks `SObject::windowStep()`, which
  is contractually try-lock (inv. 1). An implementation that reached
  `SCut::ensureReader()` would build a capture — a full render — on the UI
  thread.
- **`sounding == false` is drawn DIMMED, not hidden and not moved.** The
  cursor rests where the root was last heard. A resting cursor drawn in the
  playing colour is indistinguishable from a stalled transport.
- **Every locator-consuming edit driven from a view reads
  `localLocatorPos()`**, not the transport: `ctSplitSample` and
  `SSMVMixerControl`'s automation write tick do. `followLocator` follows the
  DRAWN cursor and does not scroll while the root is resting. The recording
  overlay is gated to the master, where both of its inputs live.

### inv. 27 — Escape closes the Clip Properties panel only while FLOATING (AC-f1)

`SClipPropertiesPanel`'s ctor installs a `QShortcut(Qt::Key_Escape)` on
ITSELF with `Qt::WidgetWithChildrenShortcut` context — not a `keyPressEvent`
override, because several of the panel's own child widgets (combo boxes,
spin boxes) consume Escape themselves before it would ever bubble up to a
widget-level override, and a shortcut with this context fires regardless of
which descendant currently holds focus. The activation handler asks its
PARENT (`qobject_cast<QDockWidget*>(parentWidget())`) whether it
`isFloating()` and calls `close()` only then — a DOCKED panel ignores Escape
entirely, exactly as it did before this existed, so a user mid-edit who taps
Escape to abandon a typed value loses nothing but the field's own revert (see
below), never the whole dock. `close()` hides the dock and leaves its stored
geometry and objectName alone, so reopening it from the View menu brings it
back floating in the same place.

This was found ALREADY IMPLEMENTED while wiring AC-f1 (commit a214c4ff,
proposal 09 M8c, landed for an unrelated reason) — nothing needed changing,
only documenting. **NOT gated**: there is no floating-window or
context-menu verb anywhere in this repo's testkit, so the floating/docked
split and "a text field's own Escape keeps its normal meaning first" (Qt's
own `QEvent::ShortcutOverride` mechanism, which a widget with something to
revert — e.g. a line edit's uncommitted text, an open combo popup — sends
before a `QShortcut` in its ancestor chain ever fires) are both hand-verified
only, by running the app on this box.
### inv. 25 — a Ctrl-drag duplicate never touches the SELECTION until release
(AC-a1)

The arm-time code (`mousePressEvent`'s primary-modifier body branch) may
mutate `clipDupItems_`/`lastClickSLink_` for its own bookkeeping, but must
NEVER call anything that changes `SApplication`'s selection list — no
`submitSetSelectionAction`, no `setSelectedSLink`, nothing. The ORIGINAL
clip(s) stay selected/highlighted for the whole drag.

Why: the live preview link `mouseReleaseEvent` drags is `delete`d before the
finalize submits anything, and `SLink`'s destroyed()→unselectSLink auto-removal
means selecting the preview at arm time silently erases whatever the user had
selected BEFORE the gesture — by the time the release snapshots "the
selection before this action" (inside `SSetSelectionAction::apply()`'s own
undo bookkeeping), it is already gone. One undo then restores an EMPTY
selection instead of the true prior one. Leaving the real selection alone
until release is what keeps that snapshot honest.

The release itself submits the new-copy `SDuplicateClipAction`(s) and exactly
ONE `SSetSelectionAction` naming the created copies, **inside the same
`beginMacro`/`endMacro` pair**, unconditionally — a single-clip copy grows the
macro too, so the step count is 1 whether one clip or a group was dragged.
Gate: `copy_drag_selects_copy.qxa`.

### inv. 26 — a manual horizontal scroll suspends follow-the-playhead for
`SMVActualView::FOLLOW_HOLD_MS` (AC-g1)

`armFollowHold()` may be called ONLY from a user-initiated pan: the wheel's
`ScrollHorizontal` case, the horizontal scrollbar's `actionTriggered` (never
`valueChanged`, which also fires when `followLocator` mirrors its own
auto-repage onto the scrollbar — arming from that signal would let the very
re-page the hold exists to delay immediately disarm it), and the
drag-past-the-canvas-edge branch of `mouseMoveEvent`. It must NEVER be called
from `setLeftOffset()` itself, `followLocator()`, or any other path that is
not directly a hand gesture, for the identical self-defeat reason.

`followLocator()` checks a monotonic `QElapsedTimer` (`followHoldTimer_`) at
the top, ahead of every other early-out, and returns without re-paging while
`followHoldArmed_` is true and less than `FOLLOW_HOLD_MS` (5000) has elapsed
since the last arm; once expired it clears the flag and behaves exactly as
before this feature. Gate: `follow_scroll_hold.qxa` (`RUN_SERIAL`, a real
wall-clock case — see its header for the measured, reproducible canvas
geometry the closed-form scrollX value depends on).

### inv. 28 — Z-ORDER IS DETERMINISTIC: LATEST START ON TOP, TIEBREAK CHILD
INDEX AND NOTHING ELSE (proposal 41 D11, M5)

`STrackRendererInline::draw()` sorts a lane's visible clips by
`(startTime, childIndex)` ASCENDING before painting, instead of walking
`childLinks()` in arbitrary insertion order. **The tiebreak is CONTRACTUAL,
not incidental**: `childIndex` is a POSITION in the lane's child list, so it
is unique across the entries being sorted and `(startTime, childIndex)` is
already a TOTAL order — nothing may be appended after it. An earlier edition
of this rule read "child index, then object id" and that was wrong twice
(`bb6b924f`, corrected the same day it landed): the third key was
unreachable, since childIndex alone already totally orders the set; and an
`SObject`'s "id" in this tree is its ADDRESS (`slink.cpp` serializes it that
way), so a comparison on it would order by whatever the allocator returned —
differing run to run, which a tiebreak this contract calls CONTRACTUAL may
never rest on.

The payoff is inv. 30's guaranteed-visible tag: a clip's left edge can only
be covered by a clip painting ABOVE it, which by this rule starts STRICTLY
LATER, i.e. strictly to the right, and so can never reach that pixel. The
single exception is an exact start-time tie, which is exactly why the
tiebreak has to be spelled out here rather than left to insertion order.

Gate: `preview_envelope_test` section 6 (a REAL `STrack` with REAL `SLink`
children painted off screen through the actual `draw()`, colours recovered
from the rendered `QImage` by RGB search — a script-level check through
`collectEnvelope` sits BELOW the paint and cannot see a paint-ORDER bug,
proposal 39 M2's finding confirmed again here, trap 6) and the qxa case
`fragment_paint_disjoint`.

### inv. 29 — A CLIP BODY PAINTS ITS MATERIAL, NOT ITS WINDOW (proposal 41
D10, M5)

A clip's body used to be one opaque `fillRect` spanning the whole placed
window. Once a fragment can place DISJOINT material on a lane (a summed
`SLaneFragment` whose children don't fill the window, or a container-backed
cut with a genuine internal silent stretch), an opaque rect over the gaps
paints a later clip's EMPTY regions over an earlier clip's MATERIAL, and the
reader sees a hole where audio is sounding.

The fix is CLIPPING, never transparency (this design uses no alpha anywhere):
per column, `STrackRendererInline::draw()` asks the clip's renderer for
`collectEnvelope()` over that column and paints only where `min != 0 ||
max != 0` — the identical "silence draws nothing" proxy proposal 39 M3's
folder-sum overlay already uses — leaving a gap column UNPAINTED so whatever
was drawn beneath it (an earlier clip, the lane background, the folder-sum
overlay) shows through. **An object whose renderer does not support
`collectEnvelope()` at all** (the base class's default `false` — an event
clip has no waveform) **keeps the ORIGINAL solid fill**: "unknown material"
must never read as "no material".

Gate: `preview_envelope_test` section 6 (the AC5.2/AC5.3 survival assertion —
verified FAILING on the pre-fix code: a later clip's gap fully erased an
earlier clip's material) and the qxa case `fragment_paint_disjoint` (the
audio-level shape only — see its header for why the paint claim itself is
not reachable from that script).

**Repaint cost, measured, not bounded** (AC5.4): a 1200×800 grab over a lane
with 6 overlapping 4 s clips (proposal 39's own corpus shape),
baseline-subtracted over 200 repeated `assert-lane-overlay` grabs, median of
3 runs: **~2.4 ms/grab before this milestone, ~3.3 ms/grab after** (+~0.9 ms,
~38%) — the added cost of per-column `collectEnvelope` + per-column
`drawLine` replacing one flat `fillRect`. M6's tag chip (inv. 30-31) adds to
the SAME paint pass; its own cost was not separately re-measured.

### inv. 30 — THE TAG CHIP: ONE OPAQUE CHIP AT BOTTOM-LEFT, GUARANTEED
VISIBLE BY inv. 28 (proposal 41 D12-D14, M6)

Every clip paints exactly ONE solid opaque chip, bottom-left corner of its
inset content rect — the corner inv. 28's z-order rule guarantees is never
covered except at an exact start-time tie (which the childIndex tiebreak
then makes deterministic). Bottom because warp markers own the top edge
(`scutrndrinline.cpp`); left because the RIGHT edge of an earlier clip is
the first thing a later one covers, which is why the OLD bottom-right
container-asset label (`SCutRendererInline`, `Qt::AlignBottom |
Qt::AlignRight`) forfeited the invariant and is now GONE — replaced, not
duplicated: `tagFullText()` is the one mechanism, reading the fragment/asset
name (`cut.getSName()`) for a container-backed clip and the file's basename
for a sample-backed one, resolved through the type-agnostic
`SClipWindow`/`SExternFile` interfaces because `objects/track` may not
depend on `objects/cut`, `objects/midi` or `objects/fragment`.

A chip rather than bare text, for the load-bearing reason proposal 39 already
recorded: anti-aliased text drawn straight on a clip body lands at every
luminance between the text and the body, which defeats a pixel gate. A chip
of known fill colour is separable; loose glyphs are not.

**Geometry is computed by ONE function, `STrackRendererInline::tagChipRect()`
— never re-derived.** `draw()` and inv. 32's hit test both call it, so paint
and hit-test cannot independently drift (the exact failure inv. 32 records
happening BEFORE this function existed). Its three collaborators are public
static functions for the same "shared decision" reason `laneFillColor()`
already is:

- `tagChipColor(laneFill)` — DERIVED from `laneFillColor()`
  (post-selection/post-`STrackColorModifier`), `.darker(160)`. The
  RELATION is contractual, never the exact hue: strictly darker (lower
  `qGray()` luminance) than the lane fill it derives from, and — because
  the fixed clip-body grey `QColor(160,160,160)` sits well above every
  reachable fill luminance (measured: base fill ~64-117, chip ~40-73) —
  comfortably darker than the clip body too, which is what keeps white
  chip text readable against it. Asserted as LUMINANCE across selection
  and every `STrackColorModifier` state, never a palette.
- `tagDensityText(fullName, fm, availTextWidthPx, cut)` — the DENSITY
  LADDER (D14): capped at `kTagMaxChars` (12) BEFORE any pixel fitting;
  full text if the capped form fits; `Qt::ElideRight` if a shorter form
  still fits; empty string if not even one character plus an ellipsis
  fits (the caller's own decision then reduces further, to "chip only" or
  "nothing"). `*cut` is set whenever the returned text is not the TRUE
  full name — the 12-char cap counts as a cut on its own, because "the cap
  is announced" (D14) means the true full name belongs on a tooltip
  whenever what is drawn is not it, not merely when pixels ran out.
- `tagFullText(clipObject)` — the text SOURCE (D12), described above.

`tagChipRect()` composes these into the actual rect `draw()` fills: chip
height is `max(fm.height() + 2*padY, kTagMinHeightPx)` clamped to the row;
chip width is the fitted text's advance plus padding, or — when no text fits
at all but the rect is still wide enough — `kTagMinChipW` (6 px, "still
reads as a mark": the CHIP-ONLY rung of D14's ladder); an empty (`isEmpty()`)
`QRect` means NOTHING fits (the row is too short, or not even the minimum
chip width). Full → elided → chip-only → nothing is therefore the ladder
`tagChipRect()`'s own three checks walk, not a separate density state
machine.

Gate: `preview_envelope_test` section 7 (colour luminance relations across
selection/`STrackColorModifier` states, the density ladder at multiple
widths, the text-source pair) and the qxa case `fragment_tag_drag` (a
canvas PNG proving both clips' chips are painted where `tagChipRect()` says
they are).

### inv. 31 — THE TAG REPLACES THE OLD LABEL; IT IS NOT A SECOND ONE
(proposal 41 D12)

`SCutRendererInline::draw()`'s old bottom-right `cut.getSName()` label is
DELETED, not left drawing alongside the new chip (`e6ae33af`,
`scutrndrinline.cpp`). Two labels naming the same clip would read as a
possible disagreement between them and would double the pixel budget a
future gate has to reason about. `tagFullText()`'s container-backed branch
reads exactly the same `cut.getSName()` the old label did, so nothing that
could be shown is lost — only WHERE it is drawn, and WHETHER it survives
`STrackColorModifier`/density pressure, changed.

### inv. 32 — HIT-TESTING FOLLOWS TAGS FIRST, THEN Z-ORDERED BODIES — NEVER
PAINT ORDER ALONE (proposal 41 D15, M7)

If the tag is the drag handle and hit-testing used paint order the way the
canvas always had, an OCCLUDED fragment's handle would be swallowed by
whatever clip painted above it — removing the entire reason the handle
exists. `SMVActualView::updateLastClickVars()` therefore tries
`tagHitTestAt()` FIRST, across EVERY clip on the lane (not just the topmost
body), and falls back to the z-ordered body test
(`STrack::getTopMostSLinkAt()`) only when no tag rect contains the point.

**This exposed a real, silent bug that had shipped since M5 and gone
undetected through a green suite for two milestones**: `STrack::
getTopMostSLinkAt()` used to return the FIRST child-order match whose
interval contained the query time — the exact insertion-order rule inv. 28
replaced for PAINTING — so paint order (inv. 28, start-time ascending) and
hit order (child-list order) silently DISAGREED the moment an
earlier-inserted clip enclosed a later-inserted, later-starting one. It now
returns the entry with the greatest `(startTime, childIndex)` among those
containing the query time — the SAME total order the paint loop sorts by —
and this fix reaches all three of its callers (press, hover cursor,
tooltip), not just the tag-priority path.

**Tag-vs-tag resolution, when more than one clip's tag rect contains the
point, is NOT the same order as body hit-testing**: ascending by start
time — the EARLIER (occludable) clip wins, protecting its WHOLE declared
chip footprint as a handle even where a later clip's body painted over part
of it, which is the actual asymmetry this milestone exists to fix. At an
EXACT start-time tie (inv. 28's own tiebreak case) the two tag rects are
pixel-identical, so there is no "partly free" clip to protect, and the
decision falls back to paint order — the higher `childIndex`, i.e. whichever
one is actually drawn on top.

**Geometry is never re-derived**: `tagHitTestAt()` calls the SAME
`STrackRendererInline::tagChipRect()` inv. 30 names, computing `vr` (the
clip's inset content rect) the identical way `draw()` does — `getXPosOfOffset()`
in the hit test, `ctx.getTimeOf()`/`visibRect` in the paint path, the same
affine map at `visibRect.x()==0`, which every lane row paints at.

Gate: the qxa case `fragment_tag_drag`, driving the REAL mouse handlers
(`click-lane`/`drag-clip-edge`'s new `edge="tag"` grab kind, computed from
the targeted clip's REAL `tagChipRect()` rather than approximated in the
script) — two same-lane clips one pixel apart: a body click deep in their
shared overlap resolves to the LATER (topmost) clip (AC7.3, the z-order
regression check), while dragging the EARLIER clip by its tag moves THAT
clip even though the later one's body painted over most of its chip
footprint (AC7.1/AC7.2). Verified as a real gate: with the tag-first wiring
reverted, the same case fails on exactly the three assertions this milestone
is about.
