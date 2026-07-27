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

How to test: lane_alignment.qxa (lane geometry + head placement under zoom,
scroll, per-track heights and take lanes), test_track_column_expansion.qxa,
test_track_width_dragging.qxa, clip_properties_actions.qxa (the property
verbs the panel submits), screenshot actions in the render cases.

Known debt: sstdmixerview is the largest file in the app and knows every
object type; per-object renderer extraction (proposal 14 slices) is the
long-term shape.
