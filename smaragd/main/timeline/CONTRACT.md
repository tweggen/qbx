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
   canvas (existing casts are debt, not precedent).
3. Gestures COMMIT through actions (e.g. resize-clip on release); live drag
   feedback may use the Raw setters but must leave the model consistent if
   cancelled.
4. All timeline math is frames at project rate; pixel↔frame conversion is
   owned by the view's zoom state.
5. **Lane geometry is per-row, and there is exactly one mapping.** A lane's
   height is `STrackRow::height` — tracks carry individual height scales
   (`setTrackHeightScale`) and one track may own several lanes (take lanes
   now, automation later). NOTHING may compute `row * trackHeight`:
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
   per tick. The sub-rect path is kept for the ONE-LANE case specifically (every
   head on a mono or stereo project); a multi-lane meter still tests before
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

How to test: lane_alignment.qxa (lane geometry + head placement under zoom,
scroll, per-track heights and take lanes), test_track_column_expansion.qxa,
test_track_width_dragging.qxa, clip_properties_actions.qxa (the property
verbs the panel submits), screenshot actions in the render cases;
meter_levels.qxa + meter_postfader.qxa (the meter's levels, its miss path and
its density rules, via the real head built off screen);
multitrack_selection.qxa (the click semantics, the mute broadcast and its
single undo step, the multi-track head drag and Group over a selection — all
through the real widgets via select-track / track-head-toggle / drag-track).

Known debt: sstdmixerview is the largest file in the app and knows every
object type; per-object renderer extraction (proposal 14 slices) is the
long-term shape.
