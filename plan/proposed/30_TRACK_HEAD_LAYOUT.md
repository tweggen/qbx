# Proposal 30: Track-head column geometry + unpainted stylesheet widgets

> **Status: EXECUTED (2026-07-26).** Root-cause analysis for two reported UI
> defects: (A) the track heads in the control column do not line up with the
> timeline lanes, and (B) a stale/black, never-repainted region in the area
> the track detail editor occupies. Both are pure UI-layer bugs; no engine,
> no threading, no persistence involvement.
>
> §A/§B below are the diagnosis as written. §C was the first-cut fix; the
> shipped one generalises it — the requester asked that the layout hold when
> tracks carry INDIVIDUAL heights and when one track owns SEVERAL lanes
> (comps, automation). That turns "one origin function" into "a row-geometry
> model", written up in **§E**, which supersedes §C.1/§C.2 (the rest of §C
> shipped as written). Execution record: `plan/STATE.md`, 2026-07-26.

## A. Track heads do not align with the lanes

### A.1 The authoritative formula, and the three that disagree with it

The lane painter is the ground truth. `SMVActualView::paintEvent`
(`main/timeline/src/sstdmixerview.cpp:244`) puts lane *i* (a row of `rows_`,
take lanes included) at

```
laneTop = SMV_TIME_RULER_HEIGHT + i*trackHeight_ - upperLeftY_
```

Three separate places place the heads, and **no two of them agree**:

| # | Site | Head y | Ruler offset | Scroll offset | Index used |
|---|------|--------|--------------|---------------|-----------|
| 1 | `rebuildControlColumn` (`:2453`, `:2460`) | `16 + h*i`, box at y=0 | once | none | row index `i` ✓ |
| 2 | `SMVActualView::setTrackHeight` (`:100`) | `h*t`, box untouched | **missing** | none | **control index `t`** ✗ |
| 3 | `setUpperLeft` / `setTopOffset` (`:123`, `:158`) | box moved to `16 - upperLeftY_`, children still at `16 + h*i` | **twice** | yes | — |

Consequences, each independently visible:

- **±16 px (`SMV_TIME_RULER_HEIGHT`) stagger.** Right after a rebuild the
  column is correct; the first vertical scroll (path 3) pushes every head
  16 px down relative to its lane; a vertical zoom (path 2) pulls them 16 px
  up. Which offset you get depends purely on which of the three ran last.
- **Whole-lane skew with take lanes expanded.** Path 2 indexes
  `controlArray_`, which deliberately has **no entry for take-lane rows**
  (`:2450` `if (row.takeRow >= 0) continue;`). After one V-zoom, every head
  below an expanded take stack is one full lane too high per take lane.
- **Scroll offset silently lost — see A.2.**

### A.2 `qTrackControlBox_` is layout-managed *and* manually positioned

`qTrackControlBox_` is added to a `QVBoxLayout` on the holder
(`:3341` `trackHolderLayout->addWidget(qTrackControlBox_, 1)`), so **the
layout owns its geometry**. Every `move()`/`resize()` the code performs on it
(`:123`, `:158`, `:2458`, `:2460`, `:2799`, `:2810`, `:3136`) is transient:
the next layout activation — window resize, dock show/hide, style change,
`setTrackControlWidth`'s `qGridLayout_->invalidate()`, a font change — snaps
the box back to `(0, 0, holderW, holderH)`.

That is the mechanism behind the *intermittent* misalignment: the lanes stay
scrolled (they scroll by repainting, from `upperLeftY_`), while the head
column jumps back to the unscrolled position, because its scroll offset lived
only in a widget position the layout is free to overwrite. The same collision
makes the box's height oscillate between `16 + h*rows_.size()` (`:2458`),
`h*rowCount()` (`:2799`, `:2810`, `:3136`) and the holder's full height — and
children outside the box are **clipped**, so heads disappear whenever the
short value wins.

### A.3 Head width is clobbered by the V-zoom path

`setTrackHeight` (`:101`) calls `mc->setFixedSize(SMV_TRACK_CTRL_WIDTH, h)` —
the hardcoded 120 px, not the user's `trackControlWidth_` (which can be up to
450). After any vertical zoom the heads are 120 px wide inside a wider column,
and `setFixedSize` pins min=max so nothing can widen them again until the next
`rebuildControlColumn`.

### A.4 The head's minimum height is frozen at construction

`SSMVMixerControl` ctor (`ssmvmixercontrol.cpp:543`):
`setMinimumSize(SMV_TRACK_CTRL_WIDTH, smv_.getTrackHeight())`. The minimum
height is whatever the track height happened to be when the control was
built. After a zoom-out-V a plain `resize(w, h)` can no longer shrink it
(only the `setFixedSize` path in A.3 can), so heads stay too tall and overlap
the lane below.

### A.5 The "compact" look itself: the strip has no short-height mode

The strip needs ≈130 px: name row (~18) + five fixed 20 px buttons
(M/S/R/T/G) + 2 px spacings + 4 px margins, plus the fader's
`setMinimumHeight(60)` in narrow mode. The default track height is 100
(`:2986`) and V-zoom-out goes to 6. Below ~130 px the grid layout overflows
and the children are simply clipped — which is exactly the truncated
`M S · T` column and the cut-off fader in the screenshot.
`SSMVMixerControl::updateLayout()` only ever branches on **width**
(`WIDE_MODE_THRESHOLD`); nothing reacts to height.

### A.6 Same origin bug in the drag/drop hit-testing

`resolveDrop` (`:2503`), `insertSlotAt` (`:2485`) and `updateTrackDrag`'s
`dropIndicator_->setGeometry(0, r*h, …)` (`:2530`, `:2536`) all map
control-box y as plain `row*h` — no ruler offset, no scroll offset, and the
indicator is drawn at a hardcoded `SMV_TRACK_CTRL_WIDTH`. So the insertion
line and the drop target are displaced from the heads by the same 16 px, and
by the scroll offset once the column is scrolled. One shared origin function
fixes the heads and this at the same time.

## B. Unpainted region where the track detail editor lives

`STrackDetailPanel` is a plain `QWidget` subclass that styles itself with

```cpp
setStyleSheet("QWidget { background-color: #2a2a2a; border-top: 1px solid #555; }");
```

and **reimplements no `paintEvent`** (`strackdetailpanel.cpp:14`). Qt requires
a `QWidget` subclass to draw the style-sheet background itself (`PE_Widget`
via `QStyleOption`, or `WA_StyledBackground`); declaring a background in the
sheet suppresses the default palette fill, so with no painter the widget's
area is never written — it keeps whatever was last in the backing store.
That is the stale/black rectangle under the dock title bar.

Three things make it a large, permanently visible rectangle rather than a
transient artifact:

1. **The panel is mostly empty.** With no selected track, `rebuildUI()` hides
   `contentWidget_` (`:88`) — so no child paints over the dead area either.
2. **It still claims 450 px.** `sizeHint()` returns
   `min(screenH/2, 450)` unconditionally (`:101`), empty or not, so the dock
   reserves a large blank region. (`heightForWidth()` is also dead code: the
   size policy never sets `setHeightForWidth(true)`.)
3. **The selector is unscoped.** `QWidget { … }` on a widget cascades to
   *every descendant*, so the plugin strip, sliders and labels all inherit
   `background-color` **and `border-top`** — cosmetic damage inside the panel
   whenever it does have content.

`STrackHeaderResizer` (the 8 px divider between the head column and the
lanes) has the identical bug: plain `QWidget`, `setStyleSheet("QWidget {…}")`,
no `paintEvent` (`strackheaderresizer.cpp:10`, `:47`, `:53`) — it is the
unpainted dark strip along the right edge of the head column.

Two smaller things in the same area, worth folding into the fix:

- `qGridLayout_->setColumnMinimumWidth(0, 8)` (`:3497`) runs *after*
  `loadTrackControlWidth()` (`:3492`) and clobbers the `width + 8` that
  `setTrackControlWidth` had just set (`:3526`).
- The resizer is added to grid cell (0,0) `Qt::AlignRight` — the *same* cell
  as `qTrackControlBoxHolder_`, so it overlays the rightmost 8 px of the
  heads instead of sitting beside them.

## C. Proposed fix

### C.1 One origin function for the control column

Make the column derive its geometry from the *same* expression the lane
painter uses, and give each control its row index so the two can never drift:

```cpp
// sstdmixerview.h
QVector<int> controlRow_;          // row index per entry of controlArray_

// sstdmixerview.cpp — the ONLY place head geometry is decided.
void SStdMixerView::layoutControlColumn()
{
    const int h  = getTrackHeight();
    const int w  = trackControlWidth_;
    const int y0 = SMV_TIME_RULER_HEIGHT - (int) qContent_->getUpperLeftY();
    for( int c = 0; c < controlArray_->size(); ++c ) {
        SSMVMixerControl *mc = controlArray_->at( c );
        if( !mc ) continue;
        mc->setGeometry( 0, y0 + controlRow_.at( c )*h, w, h );
    }
}

// Control-box y  ->  row index (inverse of the above); used by the drag code.
int SStdMixerView::rowAtBoxY( int y ) const
{
    const int h = getTrackHeight();
    return h > 0 ? ( y - SMV_TIME_RULER_HEIGHT
                       + (int) qContent_->getUpperLeftY() ) / h : 0;
}
```

Call `layoutControlColumn()` from: `rebuildControlColumn` (after creating the
controls), `SMVActualView::setTrackHeight`, `setUpperLeft`, `setTopOffset`,
`setTrackControlWidth`, and the holder's resize. Delete the per-site placement
loops in `setTrackHeight` and the `mc->move(...)` in `rebuildControlColumn`.

### C.2 Treat `qTrackControlBox_` as a fixed viewport

Stop moving/resizing it: the layout keeps it at the holder's full rect, it
clips, and the *children* carry the scroll (they already do, via `y0`).
Remove every `qTrackControlBox_->move()/resize()` call — `:123`, `:158`,
`:2458`, `:2460`, `:2799`, `:2810`, `:3136` — replacing each with a
`layoutControlColumn()`. `zoomInVert`/`zoomOutVert`/the wheel handler then
need no column code at all, since `setTrackHeight` does it.

### C.3 Let the head be exactly one lane tall and one column wide

- Drop `setFixedSize(SMV_TRACK_CTRL_WIDTH, …)` from `setTrackHeight`
  (C.1 sets the geometry).
- In `SSMVMixerControl`'s ctor, replace
  `setMinimumSize(SMV_TRACK_CTRL_WIDTH, smv_.getTrackHeight())` with
  `setMinimumWidth(TRACK_CTRL_WIDTH_MINIMAL)` and **no** minimum height, so a
  short lane is representable.
- Give the layout `qLayout_->setSizeConstraint(QLayout::SetNoConstraint)` so
  the child grid can never push the head above the lane height.

### C.4 A short mode for the strip (the "compact" complaint)

Extend `updateLayout()` to branch on height as well as width — it currently
early-returns unless the *width* mode flipped, so height changes are ignored
entirely:

```cpp
enum class Density { Full, Compact, Tiny };   // >=130, >=56, else
```

- **Full** — today's layout.
- **Compact** — buttons to 16×16 in a single horizontal row under the name,
  fader horizontal, dB readout inline with the name.
- **Tiny** — name + M/S only; everything else hidden.

Nothing is ever clipped, at any zoom.

### C.5 Route the drag geometry through the same origin

`resolveDrop`, `insertSlotAt` and `updateTrackDrag` use `rowAtBoxY()` from
C.1, and the indicator uses `trackControlWidth_` instead of the hardcoded
`SMV_TRACK_CTRL_WIDTH`.

### C.6 Make the styled widgets paint

For both `STrackDetailPanel` and `STrackHeaderResizer` — scope the selector
to the class and paint the primitive:

```cpp
setAttribute( Qt::WA_StyledBackground, true );
setStyleSheet( "STrackDetailPanel { background-color:#2a2a2a; "
               "border-top:1px solid #555; }" );

void STrackDetailPanel::paintEvent( QPaintEvent * )
{
    QStyleOption o; o.initFrom( this );
    QPainter p( this );
    style()->drawPrimitive( QStyle::PE_Widget, &o, &p, this );
}
```

(The equally valid, cheaper alternative for the resizer: drop the sheet, use
`setAutoFillBackground(true)` + a palette `Window` brush.) Scoping the
selector also stops the background/border cascading into the plugin strip.

### C.7 Don't reserve 450 px for an empty panel

In `STrackDetailPanel::rebuildUI()`, keep a placeholder `QLabel("No track
selected")` visible instead of hiding everything, and make `sizeHint()`
return a small height when `currentTrack_ == nullptr`, followed by
`updateGeometry()`. Delete the dead `heightForWidth()` override (or set
`sizePolicy().setHeightForWidth(true)`, but the sizeHint alone is enough
here).

### C.8 Divider housekeeping

Create the `STrackHeaderResizer` **before** `loadTrackControlWidth()`, drop
the stray `setColumnMinimumWidth(0, 8)` at `:3497`, and give the resizer its
own grid column (col 0 = holder, a new col for the 8 px divider) so it no
longer overlays the heads.

## E. What shipped: a row-geometry model (supersedes C.1 + C.2)

The uniform-height assumption (`lane y = row * trackHeight`) was not just the
vehicle for the bug — it is also what a per-track height or a second lane per
track would break next, in each of the ~20 places that spelled it out. So the
fix replaces the assumption rather than centralising it.

### E.1 Rows own their height

`STrackRow` gains `height` (filled by `rebuildRows`) and `isSubLane()` (today
`takeRow >= 0`; automation lanes join under the same rule). `SStdMixerView`
keeps `rowTop_`, the prefix sums, rebuilt whenever the rows, the base height
or a per-track scale change. The public surface is:

```cpp
int rowTop( int row ) const;        // column-space y; rowTop(rowCount()) = total
int rowHeight( int row ) const;
int rowAtLaneY( int y ) const;      // binary search back, -1 past the last lane
int laneGroupHeight( int row ) const;   // a track lane + its sub-lanes
int visibleRowCountFrom( int firstRow ) const;   // for the row-granular scrollbar
```

Per-track height is a **factor of the base height**
(`trackHeightScale`/`setTrackHeightScale`, clamped to 0.25..4.0, UI-only state
beside `collapsed_`/`takesExpanded_`), so vertical zoom keeps scaling every
lane uniformly and relative sizes survive it. Exposed as "Lane height ▸
Small/Normal/Large/Extra large" in the track context menu. A sub-lane's height
is its track's scale times `SUB_LANE_SCALE` (1.0 today — one edit to change,
because nothing else assumes the two match).

### E.2 One formula, in view space and column space

`SMVActualView::laneTop/laneHeight/rowAtViewY/lanesBottom` add the ruler band
and subtract the scroll; `SStdMixerView::controlYOfRow/rowAtControlY` are the
same functions for the head column (which shares the canvas' vertical origin).
Every paint, hit-test, repaint rect, drag hit-test and head placement now goes
through them — the three rival formulas of §A.1 are gone, and so is the
missing scroll term in `getSLinkVisibRect` (§A: repaint rects during a
scrolled drag).

The scroll anchor is now `topRow_` with `upperLeftY_ = rowTop(topRow_)`: a
running sum, not a multiplication, re-derived after anything that changes a
height. Heads span their **lane group**, so a track with take lanes gets one
strip covering all of them instead of a strip and a headless gap.

### E.3 The viewport stops fighting the layout

`qTrackControlBox_` is documented and treated as a fixed viewport: the
holder's layout owns its geometry, it clips, and the heads inside carry the
scroll. All seven manual `move()`/`resize()` calls on it are gone; its
`Resize` event re-places the heads instead. `SSMVMixerControl` no longer
carries a construction-time minimum height, and its layout runs with
`SetNoConstraint`, so a head can be exactly as tall as its lane.

### E.4 A test that can see it

`tests/cases/lane_alignment.qxa` walks the view through zoom, scroll,
per-track heights, take lanes, and combinations, asserting after each step
that every head sits exactly on its lane and that the row geometry inverts
cleanly (`rowAtLaneY(rowTop(i))==i` at both lane edges). Two new testkit
actions back it (`set-lane-view`, `assert-lane-alignment`), routed through
`SMainWindow` because testkit may not include `app/timeline`. The case fails
on the pre-fix code at the first scroll.

### E.5 Deliberately not done

- **C.8's "own grid column" for the divider.** The ordering fix and the
  dropped `setColumnMinimumWidth(0, 8)` shipped; the resizer still shares
  cell (0,0) with the holder, right-aligned, so it overlays the rightmost
  8 px of the heads. Moving it to its own column shifts every column index
  in the grid — worth doing, but not inside a layout bug fix.
- **Persisting per-track lane heights.** `trackScale_` is UI state like the
  fold and take-lane sets, and like them it is lost on reload. If lane
  heights should survive a save, they belong in the project file next to
  those two — one decision, all three.
- **Pruning the per-track UI state on track deletion.** `trackScale_` keys on
  `STrack*` exactly as `collapsed_` and `takesExpanded_` already do, so it
  inherits their hazard: a deleted track's entry lingers, and a later
  allocation at the same address would inherit its lane height. Pruning
  cannot simply drop keys missing from `rows_` — a track nested under a
  COLLAPSED parent is legitimately absent from the rows — so the fix is a
  walk of the model tree on structural change, for all three sets at once.
- **A drag-the-lane-bottom gesture.** The presets cover the need and the
  model does not care how the scale arrives; a drag handle is UI work that
  can land later without touching any of this.

## D. Verification

Automated: `lane_alignment.qxa` (§E.4) plus the full qxa suite, layering and
logging checks. The clip-gesture cases (`trim_start_keeps_end`,
`extend_clip_past_content`, `loop_start_edge_drag`, `render_split_slip_*`,
`takes_*`) exercise the rewritten hit-testing through the real mouse handlers.

Manual matrix — everything a headless run cannot see (nothing paints, and the
docks are never shown):

1. Scroll the track column to the bottom, then resize the window / toggle the
   Track Detail dock → heads must stay glued to their lanes (regression for
   A.2).
2. V-zoom in and out through the full range with an expanded take stack (T on
   a track that has takes) → heads stay aligned lane-for-lane, and no strip
   content is clipped (A.1 path 2, A.5).
3. Widen the head column to 450, then V-zoom → heads stay 450 wide (A.3).
4. Drag a track head across the column while scrolled → the insertion line
   lands on the boundary under the pointer (A.6).
5. Deselect all tracks (fresh project, nothing selected) and resize the
   window over the Track Detail dock → its area is uniformly #2a2a2a, no
   stale pixels (B).
