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
   per tick.
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
   (`SStdMixerView::pruneUiState`, proposal 30 §E.5, proposal 37 P6). The fold
   set, the take-lane set, the per-track height scales and the shown-automation
   set are all keyed by `STrack*`, and a removed track otherwise leaves a
   dangling key for a later track allocated at the same address to inherit —
   a new lane that mysteriously remembers a deleted one's state. The walk is
   over the MODEL, not over `rows_`: a collapsed folder's children are alive
   and have no row, and pruning against the rows would forget their state on
   every fold. It runs from `rebuildRows()`, which is the one funnel every
   structural change already passes through.

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
scroll, per-track heights and take lanes), test_track_column_expansion.qxa,
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
