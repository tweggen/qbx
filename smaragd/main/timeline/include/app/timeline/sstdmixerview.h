
#ifndef _SSTDMIXERVIEW_H
#define _SSTDMIXERVIEW_H

#include <qwidget.h>
#include <vector>

#include "tw/core/twfraction.h"
#include "tw/core/twwarpmap.h"
#include "tw/events/twtempomap.h"
#include "app/model/sobjectrenderer.h"
// Complete type for the QPointer<STrack> selection anchor below.
#include "app/objects/track/strack.h"
//#include <qptrvector.h>
#include <qtoolbutton.h>
#include <QVector>
#include <QSet>
#include <QHash>
#include <QList>
#include <QPointer>

class SStdMixer;
class QGridLayout;
class QPaintEvent;
class QMouseEvent;
class QPopupMenu;
class QScrollBar;
class STrack;
class SObject;
class SStdMixerView;
class SLink;
class QPushButton;
class QAction;
class SSMVMixerControl;

#define SMV_CUT_MIN_TIME 1024
#define SMV_LEFT_DRAG_PIXEL 7
#define SMV_RIGHT_DRAG_PIXEL 7
#define SMV_RANGE_GRAB_PIXEL 5

#define SMV_TIME_RULER_HEIGHT 16

// Per-depth horizontal indent (px) for nested track lanes/controls, and the
// width of the fold-triangle hit area drawn just left of a parent's content.
#define SMV_TRACK_INDENT 14
#define SMV_FOLD_W 12

// Width (px) of the track control strip / control column.
#define SMV_TRACK_CTRL_WIDTH 120

// One visible lane in the flattened depth-first walk of the track tree.
//
// A lane is NOT assumed to be the same height as its neighbours: `height` is
// per-row (a track may carry its own height scale, and sub-lanes may be sized
// differently from the track lane they hang off). Never compute a lane's y as
// `row * trackHeight` — ask SStdMixerView::rowTop() / SMVActualView::laneTop().
// What a row IS. `None` is the track's own composite lane; everything else is
// a SUB-LANE — it hangs off the track lane above it, carries no channel strip
// of its own, and is covered by that track's head (laneGroupHeight).
// Proposal 37 P6 (design 6.1) adds Automation beside proposal 17's Take.
enum class SubLaneKind { None = 0, Take, Automation };

struct STrackRow {
    STrack  *track;        // the track shown on this lane
    SLink   *link;         // the SLink wrapping it in its parent (timeline pos)
    SObject *parent;       // the container that holds `link` (mixer or a track)
    int      depth;        // 0 = top-level (mixer child)
    bool     hasChildren;  // has at least one child *track* (is foldable)
    bool     collapsed;    // children hidden
    // Take lanes (proposal 17 phase 3): -1 = the track's normal (composite)
    // lane; k >= 0 = the row showing take k of every take stack on the track.
    int      takeRow = -1;
    // The KIND, which is what isSubLane() reads. Take rows also carry takeRow;
    // Automation rows carry the ParamRef spelling of the lane they show and,
    // for a `param:` lane, which plugin SLOT owns it.
    SubLaneKind subKind = SubLaneKind::None;
    QString  autoTarget;
    int      autoSlotIndex = -1;
    // This lane's pixel height, filled by rebuildRows(). 0 until then.
    int      height = 0;

    // A sub-lane belongs to the track lane above it rather than standing on
    // its own (no head, not a reorder target of its own).
    bool isSubLane() const { return subKind != SubLaneKind::None; }
};

class SAutomationLaneUi;

class SMVActualView 
    : public QWidget
{
    Q_OBJECT
public:
    SMVActualView( QWidget *parent, SStdMixerView & );
    virtual ~SMVActualView();

    double getSecondWidth() const { return secondWidth_; }
    // The BASE lane height — what vertical zoom scales. It is the height of a
    // lane whose track sits at scale 1.0; it is NOT the height of any given
    // lane. Use laneHeight()/laneTop() for anything positional.
    int getTrackHeight() const { return trackHeight_; }

    // --- lane geometry: the one mapping between rows and pixels ----------
    // laneTop() is view-space (ruler band added, vertical scroll subtracted);
    // it is the formula every hit-test, repaint rect and the control column
    // derive from, so they cannot drift apart.
    int laneTop( int row ) const;
    int laneHeight( int row ) const;
    // Row under a view-space y, or -1 when the point is past the last lane.
    // A y inside the ruler band maps to the topmost visible lane (what the
    // click handlers have always done).
    int rowAtViewY( int y ) const;
    // Total pixel height of all lanes, below which the canvas is empty.
    int lanesBottom() const;
    idx_t getTopRow() const { return topRow_; }
    // ---------------------------------------------------------------------

    offset_t getUpperLeftX() const { return (offset_t) upperLeftX_; }
    offset_t getUpperLeftY() const { return (offset_t) upperLeftY_; }
    // The horizontal pan in PROJECT FRAMES — what fix/track-list-polish (m)
    // persists (SProjectProps::TimelineScrollX). getUpperLeftX() above is the
    // derived PIXEL value, which moves with the zoom; this is the value the
    // save/load round trip is exact about.
    offset_t getLeftOffset() const { return upperLeftOffset_; }
    offset_t getTimeOf( int x ) const;
    idx_t getLastClickTrackIdx() const { return lastClickTrackIdx_; }
    STrack *getLastClickTrack() const { return lastClickTrack_; }
    void resetLastClickTrack() { lastClickTrack_ = NULL; }
    // Testkit only: the group/ungroup gestures act on the last-CLICKED track,
    // and a headless run never clicks. Lets group-track drive the real slots.
    void setLastClickTrack( STrack *t ) { lastClickTrack_ = t; }
    // Show the arranger's context menu aimed at a TRACK (no clip), at a global
    // position. Used by the track heads, which have no menu of their own —
    // one menu means the track operations cannot drift apart between the two
    // places a user right-clicks a track.
    void popupTrackMenu( STrack *t, const QPoint &globalPos );
    SLink *getLastClickSLink() const { return lastClickSLink_; }
    void resetLastClickSLink() { lastClickSLink_ = NULL; }
    offset_t getLastClickOffset() const { return lastClickOffset_; }
    QPoint getLastClickPos() const { return lastClickPos_; }
    offset_t getLastClickStartOffset() const { return lastClickSelStartOffset_; }
    int getXPosOfOffset( offset_t ) const;
    QRect getSLinkVisibRect( int trackIdx, const SLink & );

    // The wheel gesture, factored out of wheelEvent() so the track-head column
    // can run the very same scroll/zoom actions (see SStdMixerView::wheelFromHead).
    // `anchorX` is the pointer's x in CANVAS coordinates, or -1 when the event
    // came from a widget that has no time axis — a horizontal zoom then keeps
    // the left edge instead of the (meaningless) time under the pointer.
    // Returns true when the gesture was consumed.
    bool applyWheel( QWheelEvent *ev, int anchorX );

    // Time-range selection (shown in the ruler band). Bounds are normalized
    // (start <= end). hasRange() is false when there is no selection.
    bool hasRange() const { return rangeValid_; }
    offset_t getRangeStart() const;
    offset_t getRangeEnd() const;

public slots:
    void setTrackHeight( int x );
    void setSecondWidth( double x );
    void setUpperLeft( offset_t upperLeftX, idx_t offsetLeftY );
    void setLeftOffset( offset_t );
    void setTopOffset( idx_t );

signals:
    void trackHeightChanged( int x );
    // Pixels per second. It was declared `int` and never emitted at all (a
    // FIXME in setSecondWidth); proposal 37 P4 needs it, because the event
    // editor's time axis follows the arranger's zoom and an int would quantise
    // every zoom step below 1 px/s.
    void secondWidthChanged( double x );
    void leftOffsetChanged( offset_t );
    void topOffsetChanged( offset_t );
    
protected slots:
    void ctGlobalShow();
    // Range-bar context menu.
    void ctRangeSetBPM();
    void ctRangeClear();
    // Track context menu: make a live asset from the right-clicked track over
    // the current ruler range (vertical scope = that track).
    void ctCreateAssetFromTrack();
    // Track context menu: expand/collapse the clicked track's take lanes.
    void ctToggleTakeLanes();
    // Clip context menu: open the clip properties panel on the selection
    // (proposal 31 — replaces the individual loop/formant/pitch items).
    void ctShowClipProperties();

protected:
    virtual void paintEvent( QPaintEvent * );
    virtual void mousePressEvent( QMouseEvent * );
    virtual void mouseDoubleClickEvent( QMouseEvent * );
    virtual void mouseMoveEvent( QMouseEvent * );
    virtual void mouseReleaseEvent( QMouseEvent * );
    virtual void contextMenuEvent( QContextMenuEvent * );
    virtual void resizeEvent( QResizeEvent * );
    virtual void wheelEvent( QWheelEvent * );
    virtual void dragEnterEvent( QDragEnterEvent * );
    virtual void dragMoveEvent( QDragMoveEvent * );
    virtual void dropEvent( QDropEvent * );
private slots:
    void globalLocatorMoved( offset_t, offset_t );
    // Keep the playhead on screen while it ADVANCES under playback/recording
    // (wired to SApplication::locatorAdvanced, never to a manual seek). Re-pages
    // the view when the cursor nears the leading edge; gated by followPlayhead_.
    void followLocator( offset_t newPos, offset_t oldPos );

private:
    void updateLastClickVars( const QPoint & );

    // Paint one take-lane row: take `row.takeRow` of every take stack on the
    // track, dimmed when inactive, highlighted when audible (phase 3).
    void drawTakeLane( QPainter &, const STrackRow &row, int rowIdx,
                       const QRect &laneRect );

    // Mouse-wheel navigation config, cached from SSettings (SOpt keys) and
    // refreshed when settings change. wheelActionFor() maps a modifier combo to
    // an SOpt::WheelAction. See wheelEvent().
    void loadWheelConfig();
    int  wheelActionFor( Qt::KeyboardModifiers mods ) const;
    QString describeWheelActions() const;  // Human-readable hint for status bar
    int  wheelPlain_, wheelShift_, wheelCtrl_, wheelCtrlShift_;
    bool wheelZoomToCursor_, wheelInvertZoom_;
    // SOpt::WheelSensitivityPct / 100, clamped, plus the three per-gesture
    // constants loadWheelConfig() derives from it. They are cached rather than
    // recomputed per event because a wheel event is a hot path, and derived in
    // ONE place because "one notch" has to mean the same amount of gesture in
    // all four. At 100 % each is bit-for-bit the value it was hard-coded to.
    double wheelSensitivity_;
    int    wheelVScrollStep_;   // angleDelta units per track lane
    double wheelZoomHFactor_;   // px/second multiplier per notch
    double wheelZoomVFactor_;   // track-height multiplier per notch
    // Accumulated vertical-scroll wheel delta (angleDelta units). A trackpad or a
    // Magic Mouse delivers many tiny sub-notch deltas; stepping a whole lane per
    // event made vertical scroll wildly over-sensitive. We accumulate and step one
    // lane per SMV_WHEEL_VSCROLL_STEP units instead (see wheelEvent).
    int  wheelVScrollAccum_ = 0;
    // View-follows-playhead (SOpt::FollowPlayhead), cached alongside the wheel
    // config and refreshed on SSettings::changed. See followLocator().
    bool followPlayhead_;
    // The x column where paintEvent last drew the playhead (-1 = off-screen /
    // not yet drawn). globalLocatorMoved erases THIS column rather than a
    // position-derived one, so an RT-advanced locator can't leave a ghost line.
    int lastPaintedCursorX_ = -1;

    // --- time-range selection -------------------------------------------
    enum RangeDrag { RangeNone, RangeCreate, RangeMoveStart, RangeMoveEnd, RangeMove };
    void beginRangeDrag( int x );    // mouse press in the ruler band
    void updateRangeDrag( int x );   // mouse move while dragging
    void endRangeDrag( int x );      // mouse release
    void rangeBounds( offset_t &lo, offset_t &hi ) const;  // normalized
    void drawRange( QPainter &, const QRect &myRect );
    void drawRulerTicks( QPainter &, const QRect &myRect );
    void saveRangeToProject();
    void loadRangeFromProject();
    // --------------------------------------------------------------------

    // --- zoom / horizontal pan persistence (fix/track-list-polish m) -----
    // Restores what a previous save left in the project's property dict, or
    // this widget's own hard-coded defaults when there is nothing saved (a
    // fresh project, or one written before this existed). Saved on every
    // change via secondWidthChanged/leftOffsetChanged so every path that
    // moves them — wheel, the zoom buttons, the scrollbar — is covered
    // without hunting down each call site.
    void saveViewStateToProject();
    void loadViewStateFromProject();
    // --------------------------------------------------------------------

    SStdMixerView &smv_;

    class InlineRenderContext
        : public SRenderContext {
    public:
        InlineRenderContext( SMVActualView &, QPainter & );
        virtual ~InlineRenderContext();
        
        SMVActualView &getMixerView() const { return mixerView_; }
        
        virtual offset_t getTimeOf( int x ) const;
    private:
        SMVActualView &mixerView_;
    };

    int upperLeftX_;
    // Vertical scroll, in pixels, derived from topRow_: scrolling is still
    // row-granular, but with variable lane heights the pixel offset is the
    // running sum up to topRow_, not a multiplication. Keep the two in sync
    // through setTopOffset()/setUpperLeft() only.
    idx_t upperLeftY_;
    idx_t topRow_ = 0;
    offset_t upperLeftOffset_;

    int trackHeight_;
    double secondWidth_;
    QMenu *qGlobalPopup_;
    // "Lane height" submenu of the track context menu, built on first use and
    // kept (qGlobalPopup_->clear() would otherwise leak one per right-click).
    QMenu *qLaneHeightMenu_ = nullptr;
    QMenu *qRangePopup_;
    QAction *qRangeActClear_;
    bool rangeValid_;
    offset_t rangeStart_, rangeEnd_;   // the two ends (not necessarily ordered)
    int rangeDrag_;                    // RangeDrag
    // RangeMove snapshot: press time and original bounds, so a body-drag shifts
    // the whole selection by (now - press) while preserving its length.
    offset_t rangeMovePressT_ = 0, rangeMoveAnchorStart_ = 0, rangeMoveAnchorEnd_ = 0;
    int lastClickTrackIdx_;
    STrack *lastClickTrack_;
    SLink *lastClickSLink_;
    offset_t lastClickOffset_;
    QPoint lastClickPos_;
    offset_t lastClickSelStartOffset_;
    bool lastClickedStart_, lastClickedEnd_;
    bool lastClickedEndUpper_ = false;   // right edge band AND cursor in upper lane half
    bool lastClickedStartUpper_ = false; // left edge band AND cursor in upper lane half
    length_t lastClickDuration_;

    // Loop-marker grab handles. loopMarkerAt() hit-tests the small boxes the cut
    // renderer draws at the top of each loop boundary (scutLoopHandleRect() is
    // shared with the drawing side) and returns the boundary index under `pos`,
    // or 0 for none. Grabbing boundary k and dragging re-tiles the clip so that
    // boundary follows the pointer: segment = (t - clipStart)/k. The clip's
    // duration is NOT touched, so the repetition count changes as you drag.
    int loopMarkerAt( const QPoint &pos, int rowIdx, SLink *clip ) const;
    int lastClickLoopMarker_ = 0;   // boundary index under the last press (0=none)

    // Clip MOVE drag snapshot: captured at press so the move lands as one
    // undoable SMoveClipAction on release (the drag itself mutates live).
    bool     clipDragArmed_ = false;
    STrack  *clipDragTrack0_ = nullptr;
    offset_t clipDragStart0_ = 0;
    // SCut start-offset at press, for the resize (edge-drag) undo snapshot.
    offset_t clipResizeOffset0_ = 0;
    Fraction clipSrcStart0_ = Fraction(0);  // exact pre-drag source anchor
    // Loop length / grain stretch at press, for the loop & time-stretch gestures.
    length_t clipLoopLen0_ = 0;   // original loop length (revert snapshot)
    length_t clipLoopSeg_  = 0;   // loop segment captured during a loop drag
    Fraction clipStretch0_ = Fraction(1);
    // Which clip-edit gesture this drag is: Alt-body = slip, Ctrl-border = stretch,
    // right-upper edge = loop. (Resize/extend/trim use lastClickedStart_/End_.)
    bool clipDragIsSlip_ = false;
    bool clipDragIsStretch_ = false;
    bool clipDragIsLoop_ = false;
    // Take-lane slip (proposal 17 UI gap): -1 = the drag targets lastClickSLink_'s
    // own SObject directly (the ordinary composite-lane case). >= 0 = the drag was
    // armed by an Alt-click on a TAKE LANE row; lastClickSLink_ stays the take
    // STACK's outer link (position/duration/path all still come from it), but the
    // SCut actually being slipped is stack->takeAt(clipDragTakeIndex_) — the take
    // under the pointer, which need not be the active/comped one.
    int clipDragTakeIndex_ = -1;
    // W2 warp-marker drag (proposal 28): armed by a press in the marker
    // strip (top pixels of a clip) near a handle; the drag mutates live via
    // SCut::setWarpAnchors, release reverts to the press snapshot and
    // submits one undoable SMoveWarpMarkerAction (the house revert-then-
    // action pattern).
    bool                      markerDragArmed_ = false;
    int64_t                   markerDragSrc_   = 0;
    int64_t                   markerDragPreStartOffset_ = 0;
    std::vector<twWarpAnchor> markerDragPre_;
    bool tryBeginMarkerDrag( QMouseEvent *ev );
    void updateMarkerDrag( QMouseEvent *ev );
    void finishMarkerDrag();
    bool tryAddMarkerAt( QMouseEvent *ev );
    bool clipDragIsLoopMarker_ = false;   // grabbed a loop marker (re-tile)
    // Left edge, upper half: extend/shrink the clip BACKWARDS in whole loop
    // cycles. Whole cycles is not a UI nicety — twLoopMap sends clip-relative p
    // to base + (p mod len), so only a shift that is a multiple of len leaves
    // every existing repetition on the same timeline frame. Any other shift
    // moves the wrap point and rewrites the audio.
    bool clipDragIsLoopStart_ = false;
    // Telegraph the gesture on hover. Modifiers come from the event, matching
    // what mousePressEvent will act on.
    void updateHoverCursor( const QPoint &pos, Qt::KeyboardModifiers mods );

    // Ctrl-drag DUPLICATE: when armed, the dragged clips are live copies and the
    // release submits SDuplicateClipAction(s) instead of a move. Duplicates every
    // selected clip; the clicked clip is the anchor and the rest follow by the
    // same time/row delta. syncDuplicateGroup() moves the non-anchor copies.
    bool clipDragIsDuplicate_ = false;
    struct ClipDupItem {
        SLink     *copy = nullptr;   // live preview copy being dragged
        QList<int> sourcePath;       // path of the original clip
        offset_t   origStart = 0;    // original start time of this clip
        int        origRow = -1;     // original lane row of this clip's track
    };
    QVector<ClipDupItem> clipDupItems_;
    offset_t   clipDupAnchorStart_ = 0;   // anchor copy's start at press
    int        clipDupAnchorRow_   = -1;  // anchor's lane row at press
    void syncDuplicateGroup();            // move non-anchor copies with the anchor
};

class STimeGridSpec {
public:
    void setTimeGridWidth( double width ) { timeGridWidth_ = width; }    
    double getTimeGridWidth() const { return timeGridWidth_; }
    int getEmphasizeGrids( int i ) const { return (i>=0 && i<4)?emphasizeGrids_[i]:0; }
    void setEmphasizeGrids( int idx, int val ) { if( idx>=0 && idx<=3 ) emphasizeGrids_[idx] = val; }
    double getBPM() const; 
    void setBPM( double );
private:
    double timeGridWidth_;
    short emphasizeGrids_[4];
};

class SSnapSpec
    : QObject
{
    Q_OBJECT
public:
    SSnapSpec( STimeGridSpec & );
    virtual ~SSnapSpec();

    enum {
        NoSnap=0,
        SnapToBeats=1,
        SnapToEvents=2
    };

    virtual offset_t alignTime( offset_t );

    idx_t getBeatSubDiv() const;
    int getSnapMethod() const;
    void setSampleRate( int srate ) { sampleRate_ = srate; }

    /**
     * GRID DIVISIONS (proposal 37 P4). The subdivision used to be an unexposed
     * `beatSubDiv_` that alignTime never read; a division is now named the way
     * the whole app names one - "1/1".."1/32", with a trailing `t` for triplets
     * - and parsed by SQuantizeNotesAction::gridTicks(), the ONE parser, so
     * `quantize-notes grid=`, the event editor's grid and the arranger's snap
     * cannot mean different things by the same string.
     *
     * An empty division restores the pre-36 behaviour exactly (snap to the beat
     * width the STimeGridSpec carries), which is what keeps every committed qxa
     * case's snapped positions unchanged.
     */
    void setGridDivision( const QString &division );
    QString gridDivision() const { return gridDivision_; }

    /**
     * The TEMPO MAP is the single tempo authority (D2), so a division is
     * converted to frames through it and not by multiplying 60/bpm. Without
     * one set, alignTime falls back to the grid spec's beat width.
     */
    void setTempoMap( const twTempoMap &map );

signals:
    void snapMethodChanged( int );
    void beatSubDivChanged( idx_t );

public slots:
    void setBeatSubDiv( idx_t );
    void setSnapMethod( int );
protected:
private:
    /** The snap step in frames, or 0 when there is no usable division. */
    offset_t divisionFrames_() const;

    idx_t beatSubDiv_;
    int snapMethod_;
    int sampleRate_;
    STimeGridSpec &tgs_;
    QString    gridDivision_;
    twTempoMap tempoMap_;
    bool       haveTempoMap_ = false;
};

class SStdMixerView
    : public QWidget
{
    Q_OBJECT
public:
    SStdMixerView( QWidget *parent, SStdMixer *model );
    virtual ~SStdMixerView();

    SStdMixer *getModel() const
        { return model_; }

    STimeGridSpec getTimeGridSpec() const { return timeGridSpec_; }

    // The time-axis canvas. Exposed for the SHELL only (proposal 37 P4): the
    // event editor's SEventTimeAxis follows this widget's zoom and scroll, and
    // the shell is the one module that sees both app/timeline and app/eventui.
    SMVActualView *contentView() const { return qContent_; }

    // The snap spec (grid divisions since proposal 37 P4). Null before the
    // view is fully constructed.
    SSnapSpec *snapSpec() const { return currentSnapSpec_; }

    SLink *ensureSCut( SLink * );

    // TEST ENTRY POINT: drive a real clip-edge gesture through the arranger's
    // own mouse handlers (press → move → release) as a user drag does. Every
    // clip-edge clamp lives in mouseMoveEvent, which no action can otherwise
    // reach, so the qxa suite has no other way to cover trim / extend / loop /
    // loop-marker behaviour. `clipIdx` counts only real clips (nested track
    // lanes are skipped); `grabEnd` picks the right edge over the left;
    // `upperHalf` picks the loop half of the edge band over the extend half.
    // `mods` are delivered ON THE EVENT, so modifier gestures (Ctrl-stretch,
    // Alt-slip, Ctrl-duplicate) are drivable too — the handlers read
    // ev->modifiers() rather than the live keyboard. `grabWhere` picks where the
    // press lands: GrabBody is what the body gestures (slip, duplicate, move)
    // need, since a press inside an edge band can never arm them.
    enum ClipGrab { GrabStart = 0, GrabEnd = 1, GrabBody = 2 };
    // Testkit: run the REAL Group/Ungroup context-menu slots on `t`. The index
    // arithmetic in those slots is the whole risk, so it must be the thing
    // under test rather than a re-spelling of it in a qxa script.
    bool groupGesture( STrack *t, bool ungroup );
    bool dragClipEdge( int rowIdx, int clipIdx, int grabWhere, offset_t dropTime,
                       bool upperHalf, Qt::KeyboardModifiers mods = Qt::NoModifier );

    // TEST ENTRY POINT: drive a real double-click on a clip through the
    // arranger's own mouse handlers (drag-clip-edge's twin). Lands on the
    // clip BODY, exactly as SMVActualView::mouseDoubleClickEvent expects for
    // its "open the event editor" branch — an EVENT clip opens it, any other
    // clip is a no-op success (matches production: nothing else happens).
    bool doubleClickClip( int rowIdx, int clipIdx );
    // TEST ENTRY POINT: a single, real click (press+release, no move) into
    // track `t`'s LANE — the clip-drawing area, as opposed to its head — at
    // `time`. Lands on whatever is there: a clip if one covers that position
    // (same as clicking its body), empty lane space otherwise. Exercises the
    // lane-click track selection and no-drag cursor positioning that live in
    // SMVActualView::mousePressEvent/mouseReleaseEvent, the same handlers
    // dragClipEdge drives — this is its "click on the lane itself" twin, and
    // it is the only route to a lane click that hits no clip at all.
    bool clickLane( STrack *t, offset_t time,
                    Qt::KeyboardModifiers mods = Qt::NoModifier );

    // TEST ENTRY POINTS for the multi-track selection. A headless run never
    // clicks, and the two things worth covering are exactly the click
    // semantics (plain / Ctrl / Shift) and the BROADCAST from a head's toggle
    // button — a script that set the model's selection and called
    // set-track-mute per track would test the script, not the feature. Same
    // shape and rationale as drag-clip-edge and group-track.
    bool tkClickTrackHead( STrack *t, Qt::KeyboardModifiers mods );
    bool tkToggleTrackHead( STrack *t, const QString &which, bool on );
    // Send a synthetic Home/End key press to `t`'s LIVE fader (item j) — the
    // same widget instance the real arranger draws, found in controlArray_
    // exactly as tkToggleTrackHead's button does. Exercises
    // SSMVMixerControl::eventFilter's Home/End interception without needing
    // real OS-level keyboard focus under QT_QPA_PLATFORM=offscreen.
    bool tkSendFaderKey( STrack *t, const QString &key );
    // Grip-drag `t`'s head and drop it on a LANE ROW: `nestOnto` drops on the
    // middle of that row (nest into it), otherwise on its top boundary (insert
    // above it; `targetRow` == rowCount() means "past the last lane"). Rows,
    // not pixels, so the script does not encode the current zoom — but the
    // real beginTrackDrag/updateTrackDrag/endTrackDrag run, which is where the
    // multi-track move arithmetic lives.
    bool tkDragTrackHead( STrack *t, int targetRow, bool nestOnto );

    // TEST ENTRY POINTS for the lane geometry. The heads are placed by
    // layoutControlColumn() while the lanes are drawn by the canvas; nothing
    // else can prove the two agree, and they only ever disagreed *after* some
    // event moved one of them — hence the knobs to move things first.
    void tkSetBaseTrackHeight( int h );      // vertical zoom, absolute
    void tkSetTopRow( int row );             // vertical scroll, in lanes
    // "" when every head sits exactly on its lane (same top, same height as
    // the painter uses, full column width), otherwise the first mismatch.
    QString tkCheckLaneAlignment() const;
    // TEST ENTRY POINT (fix/track-list-polish l): the vertical scrollbar's own
    // `maximum()`, in row-index units — the number `verticalScrollMaximum()`
    // computes and the real mouse wheel is capped by. `tkSetTopRow()` above
    // does NOT reproduce the bug the padding fixes: it clamps only to
    // `rowCount()-1` and bypasses the scrollbar's maximum entirely, so a case
    // that wants to test what a wheel scroll can actually reach must read
    // this number and scroll to IT, not to `rowCount()-1`.
    int tkVerticalScrollMaximum() const;
    // The automation half of that check (proposal 37 P6), defined in
    // sautomationlane.cpp: every Automation row must still name a lane the
    // model can resolve. A row left behind by a removed lane paints an empty
    // band indistinguishable from a flat one.
    QString checkAutomationRows() const;

    offset_t alignTime( offset_t );

    // The BASE lane height (vertical zoom scales this). Individual lanes may
    // differ — see rowHeight().
    int getTrackHeight() const;

    // --- flattened track-tree (depth-first) the arranger draws -----------
    // The view shows one lane per STrackRow. rows_ is rebuilt by refreshTrackTree
    // whenever the tree or a fold state changes; everything that used to index
    // the flat mixer (paint, hit-testing, drag, scrolling, the control column)
    // now indexes these rows.
    int rowCount() const { return rows_.size(); }
    const STrackRow *rowAt( int i ) const;

    // --- lane geometry (column space; add the ruler / subtract the scroll
    // via SMVActualView::laneTop() to reach view space) -------------------
    // rowTop( rowCount() ) is the total height, so the two are one call.
    // Heights are per-row: a track can be taller than its neighbours and a
    // track can own several lanes. Nothing may assume `row * trackHeight`.
    int rowTop( int row ) const;
    int rowHeight( int row ) const;
    int totalRowsHeight() const { return rowTop( rows_.size() ); }
    // How many lanes fit in the canvas starting at `firstRow` (>= 1). The
    // vertical scrollbar is row-granular, so its page step depends on which
    // rows are on screen once heights differ.
    int visibleRowCountFrom( int firstRow ) const;
    // The `qScrollVert_` maximum for a given page step `vis` (fix/
    // track-list-polish l). `visibleRowCountFrom()` can count a row that only
    // PARTIALLY fits as fully visible whenever the content does not divide
    // evenly into the viewport, which without correction leaves the true
    // last row permanently clipped at the bottom with no scroll room left to
    // reach it. This reserves roughly one track's height of scroll headroom
    // below the real content whenever it is taller than the viewport (never
    // when it already fits, so scrolling stays disabled when there is
    // nothing to scroll for). ONE definition, used by both recalcPageStep()
    // (a resize/zoom/height-scale change) and nTracksChanged() (a track
    // added/removed), so the two cannot compute two different answers.
    int verticalScrollMaximum( int vis ) const;
    // Row containing column-space y, or -1 past the last lane.
    int rowAtLaneY( int y ) const;

    // Per-track lane height, as a factor of the base height (UI-only state,
    // like the fold and take-lane sets). Relative to the base so that
    // vertical zoom keeps working uniformly. 1.0 = the plain lane height.
    static constexpr double LANE_SCALE_MIN = 0.25;
    static constexpr double LANE_SCALE_MAX = 4.0;
    double trackHeightScale( const STrack * ) const;
    void setTrackHeightScale( STrack *, double scale );
    int rowIndexOfTrack( const STrack * ) const;
    // Fold state now lives ON the track (STrack::isCollapsed(), fix/
    // track-list-polish m) so it survives save/load and needs no pruning of
    // its own — it dies with the object instead of outliving it in a
    // view-owned QSet. toggleTrackCollapsed() still owns the row rebuild.
    bool isTrackCollapsed( STrack *t ) const { return t && t->isCollapsed(); }
    void toggleTrackCollapsed( STrack * );
    // Take lanes (proposal 17 phase 3): per-track expanded state, UI-only.
    // An expanded track shows one extra row per take index below its lane.
    bool isTrackTakesExpanded( STrack *t ) const
        { return takesExpanded_.contains( t ); }
    void toggleTrackTakesExpanded( STrack * );

    // --- automation lanes (proposal 37 P6, design 6.1) -------------------
    // The painting, the gestures, the picker menu, the shown-lane set and the
    // testkit driver all live in timeline/src/sautomationlane.{h,cpp}; so do
    // the definitions of the five members below. That file is the whole
    // feature, and sstdmixerview.cpp keeps only the CALL SITES — this file is
    // already the largest in the app (CONTRACT, known debt).
    SAutomationLaneUi &automationUi();
    // ONE pruning walk for EVERY per-track UI-state set — the fold set, the
    // take-lane set, the per-track height scales and the shown-automation set
    // (proposal 30 section E.5). Called from rebuildRows(), so a removed track
    // cannot leave a dangling STrack* key behind for a later track allocated
    // at the same address to inherit.
    void pruneUiState();
    void appendAutomationRowsFor( STrack *tk, SLink *lk, SObject *container,
                                  int depth );
    // Show / hide one automation lane on a track (the picker's and the
    // testkit's one entry point). Returns false only for a bad argument.
    bool showAutomationLane( STrack *t, const QString &target, int slotIndex,
                             bool show );
    // Arm clip-envelope (`cut:Gain`) editing. OFF by default, and that is
    // load-bearing: while it is off every clip-body gesture — move, slip,
    // duplicate, stretch — behaves exactly as it always has.
    void setClipEnvelopeEdit( bool on );
    // Testkit: the drag-clip-edge twin for an automation breakpoint. `owner`
    // is the index-path spelling every automation verb takes.
    bool dragAutomationPoint( const QString &owner, const QString &target,
                              int slotIndex, int take, offset_t time,
                              double value, offset_t toTime, double toValue,
                              Qt::KeyboardModifiers mods );
    // --------------------------------------------------------------------

    void refreshTrackTree();   // rebuild rows + control column + relayout + repaint
    // --------------------------------------------------------------------

    // --- multi-track selection -------------------------------------------
    // The arranger owns the selection GESTURES; the set itself lives on the
    // model (SStdMixer). One rule governs every track-level operation below:
    //
    //   a gesture aimed at a track that is PART of the selection acts on the
    //   whole selection; a gesture aimed at any other track acts on that track
    //   alone (and, for a click, replaces the selection with it).
    //
    // Otherwise "mute" on an unselected lane would silently hit lanes the user
    // is not pointing at.
    //
    // Modifiers on a head click: plain = select just this one, Ctrl = toggle
    // membership, Shift = select the lane range from the anchor (the last
    // plainly-clicked or Ctrl-clicked head) to this one. Read from the EVENT,
    // never from the live keyboard — same rule the clip gestures follow.
    void applyTrackSelectionClick( STrack *t, Qt::KeyboardModifiers mods );
    // Every visible lane's track between `a` and `b` inclusive, in lane order.
    QList<STrack *> tracksBetween( STrack *a, STrack *b ) const;
    // The tracks a gesture aimed at `clicked` acts on, in LANE ORDER (top
    // down). Empty when `clicked` is NULL.
    QList<STrack *> selectionTargets( STrack *clicked ) const;
    // `in`, minus every track that already has an ancestor in `in`. Structural
    // operations take the outermost track only — reparenting or removing a
    // folder carries its subtree, so acting on a child as well would either
    // double-apply or tear the subtree apart.
    static QList<STrack *> pruneNestedTargets( const QList<STrack *> &in );
    // Sort `in` by lane position. Order decides insertion slots, so every
    // multi-track operation sequences by it rather than by click order.
    QList<STrack *> orderByLane( const QList<STrack *> &in ) const;
    // The lane Ctrl+T's new track goes below: the LAST selected one by lane
    // position, falling back to the last lane the user aimed at. NULL when
    // there is nothing to be below. The CONTEXT MENU does not use this — it
    // aims at the lane it was opened on (SMVActualView::ctGlobalShow).
    STrack *newTrackReference_() const;
    // Insert a new track immediately BELOW `ref`, as its next sibling in the
    // same container. `ref == NULL` appends at the end of the arrangement,
    // which is what this used to do unconditionally.
    void addTrackBelow_( STrack *ref );
    // Pop the arranger's track context menu for `t` at a global position (the
    // track heads have no menu of their own — this is the same menu the
    // timeline canvas shows, aimed at a head).
    void showTrackContextMenu( STrack *t, const QPoint &globalPos );
    // Route a wheel event that landed on the track-head column (a head, one of
    // its child widgets, or the blank area below the last head) into the
    // arranger canvas, so the configured scroll/zoom gestures work there too.
    // Returns true when the gesture was consumed.
    bool wheelFromHead( QWheelEvent *ev );

    // Track-reorder drag, driven by a control's grip handle. beginTrackDrag()
    // arms the drag for the given control; updateTrackDrag()/endTrackDrag() take
    // a Y in qTrackControlBox_ coordinates. On release a move past the original
    // slot submits an SMoveTrackAction.
    void beginTrackDrag( SSMVMixerControl *control );
    void updateTrackDrag( int yInControlBox );
    void endTrackDrag( int yInControlBox );

public slots:
    void ctAddTrack();
    // Add a new track whose parent is the same container as the last visible
    // lane's track (the "track above" the blank area). Triggered by a double
    // click in the blank space below the track heads, and by a media-browser
    // drop landing on that same blank area. Routes through the undoable
    // SAction system. Returns the new track (still a valid pointer after the
    // reparent macro, which moves the SLink but never re-creates the STrack),
    // or NULL if there is no project to add to.
    STrack *ctAddTrackBelowLast();
    void ctRemoveTrack();
    // Grouping via the right-click menu / toolbar.
    void ctIndentTrack();    // nest the clicked track under its preceding sibling
    void ctOutdentTrack();   // move it out to its grandparent, after its parent
    void ctGroupTrack();     // wrap the clicked track in a new folder track
    void ctUngroupTrack();   // dissolve the clicked folder, promoting its children
    void ctInsertSample();
    void ctRemoveSample();
    void ctDeleteSample();
    void ctSplitSample();
    // Transpose the selected clips (or, with an empty selection, the
    // last-clicked one) by a signed offset in CENTS. One undo step per press.
    void ctPitchUp()       { nudgeClipPitch(  100.0 ); }
    void ctPitchDown()     { nudgeClipPitch( -100.0 ); }
    void ctPitchUpFine()   { nudgeClipPitch(   10.0 ); }
    void ctPitchDownFine() { nudgeClipPitch(  -10.0 ); }
    void ctAddLink();
    void setTimeGridSpec( const STimeGridSpec & );    
    // The project's tempo moved (bpmTempoChanged): re-derive the ruler grid.
    // A LISTENER, not a writer - the only tempo write is the set-tempo verb.
    void onProjectTempoChanged( double );
    // void scrollTo( QPoint &x );
    
signals:
    void timeGridSpecChanged( const STimeGridSpec & );

protected:
    enum {
	HSliderRange = 2000
    };
    void recalcPageStep();
    // Watches qTrackControlBoxHolder_ for double-clicks in its blank area
    // (below the track heads) to spawn a new track.
    bool eventFilter( QObject *watched, QEvent *event ) override;

    // Track header resizing via draggable divider
    void mousePressEvent( QMouseEvent *event ) override;
    void mouseMoveEvent( QMouseEvent *event ) override;
    void mouseReleaseEvent( QMouseEvent *event ) override;
    void updateDividerCursor( const QPoint &pos );

protected slots:
    void viewResized();

private slots:
    void contentDurationChanged( length_t newDuration );
    void nTracksChanged();
    // Clip-level edits (add-take/remove-take) change the take-row count of an
    // expanded track without any track-structure signal; resync the rows on
    // every applied action, cheaply when nothing changed.
    void onArrangementChangedRows();
    void timeSliderMoved( int newValue );
    void trackSliderMoved( int newValue );
    void zoomOutHor();
    void zoomInHor();
    void zoomOutVert();
    void zoomInVert();
    void avLeftOffsetChanged( offset_t );
    void addMixerControl( int, STrack & );
    void removeMixerControl( int, STrack & );
    // Re-sequence the control column to match the model's (reordered) track
    // order without adding/removing controls.
    void tracksReordered();

private:
    // Transpose every target clip by `cents`, as ONE undoable step: each clip
    // gets its OWN absolute target (its current pitch + cents), so clips that
    // sat at different pitches stay different. Targets are the current
    // selection, or the last-clicked clip when nothing is selected.
    void nudgeClipPitch( double cents );

    // Symbolic constants for the screen layout.
    enum {
        GLROW_TOTAL_ZOOM = 3,
        GLCOL_TOTAL_ZOOM = 4,

        GLROW_VZOOM_OUT = 0,
        GLCOL_VZOOM_OUT = 4,
        GLROW_VSCROLL = 1,
        GLCOL_VSCROLL = 4,
        GLROW_VZOOM_IN = 2,
        GLCOL_VZOOM_IN = 4,       
        
        GLROW_HZOOM_OUT = 3,
        GLCOL_HZOOM_OUT = 1,
        GLROW_HSCROLL = 3,
        GLCOL_HSCROLL = 2,
        GLROW_HZOOM_IN = 3,
        GLCOL_HZOOM_IN = 3,
        
        GLROWSTART_CONTENT = 0,
        GLROWSTOP_CONTENT = 2,
        GLCOLSTART_CONTENT = 1,
        GLCOLSTOP_CONTENT = 3,
        
        GLROWSTART_TRKCTRL = 0,
        GLROWSTOP_TRKCTRL = 3,
        GLCOLSTART_TRKCTRL = 0,
        GLCOLSTOP_TRKCTRL = 0,

        GLROWSTRETCH_0 = -1,
        GLROWSTRETCH_1 = 100,
        GLROWSTRETCH_2 = -1,
        GLROWSTRETCH_3 = -1,

        // Column 0 (track control): allow expansion up to max width constraint (a)
        GLCOLSTRETCH_0 = 1,
        GLCOLSTRETCH_1 = -1,
        GLCOLSTRETCH_2 = 100,
        GLCOLSTRETCH_3 = -1,
        GLCOLSTRETCH_4 = -1
    };
    
    friend class SMVActualView;

    QGridLayout *qGridLayout_;
    QScrollBar *qScrollVert_;
    QScrollBar *qScrollHoriz_;   
    SMVActualView *qContent_;
    QToolButton *qHZoomIn_, *qHZoomOut_;
    QToolButton *qVZoomIn_, *qVZoomOut_;
    QToolButton *qZoomTotal_;

    SStdMixer *model_;

    // Persistent actions so their keyboard shortcuts work whenever the arranger
    // is up (a context-menu-only action's shortcut never fires). Also shown in
    // the right-click menu.
    QAction *actNewTrack_;       // Ctrl+T
    QAction *actInsertSample_;   // Ctrl+Return
    QAction *actSplit_;          // S
    QAction *actRemoveSample_;   // Delete
    QAction *actPitchUp_;        // +   (one semitone)
    QAction *actPitchDown_;      // -
    QAction *actPitchUpFine_;    // Shift++ (10 cents)
    QAction *actPitchDownFine_;  // Shift+-

    bool snapToTimeGrid_;
    STimeGridSpec timeGridSpec_;
    SSnapSpec *currentSnapSpec_;

    // qTrackControlBox_ is a fixed VIEWPORT: the holder's layout owns its
    // geometry and it never moves. The heads inside it carry the scroll
    // offset (see layoutControlColumn) and are clipped by it. Moving the box
    // instead would be silently undone by the next layout activation.
    QWidget *qTrackControlBoxHolder_;
    QWidget *qTrackControlBox_;
    QVector<SSMVMixerControl*> *controlArray_;
    // Row index of controlArray_[i]. Sub-lane rows carry no head, so the two
    // vectors are NOT index-parallel with rows_ — never assume they are.
    QVector<int> controlRow_;

    // Flattened track tree + per-track fold state (UI-only).
    QVector<STrackRow> rows_;
    // Prefix sums of the row heights: rowTop_[i] is the top of row i in
    // column space, rowTop_[rowCount()] the total. Rebuilt with rows_ and
    // whenever the base height or a per-track scale changes.
    QVector<int> rowTop_;
    QSet<STrack*> takesExpanded_;   // tracks showing their take lanes
    QHash<const STrack*, double> trackScale_;   // per-track lane height factor
    // The automation UI (proposal 37 P6). Created lazily by automationUi() so
    // a view that never shows a lane pays nothing.
    SAutomationLaneUi *autoUi_ = nullptr;
    void rebuildRows();
    void rebuildRowGeometry();      // recompute row heights + rowTop_
    int  rowHeightOf( const STrackRow & ) const;
    void rebuildControlColumn();

public:
    // Place every head at its lane: y = laneTop(row), height = the lane group
    // (the track lane plus any sub-lanes hanging off it). THE single place
    // head geometry is decided — call it after anything that moves a lane.
    void layoutControlColumn();
    // Column-space y of a row's top / the row under a column-space y. The
    // control column shares the canvas' vertical origin, so these are
    // laneTop()/rowAtViewY() by another name; the drag code speaks this space.
    int controlYOfRow( int row ) const;
    int rowAtControlY( int y ) const;
    // Height of the head for the track lane at `row`: that lane plus every
    // sub-lane attached to it, so a track owning several lanes gets one head
    // spanning all of them.
    int laneGroupHeight( int row ) const;

    // Track header resizing (Phase 3 UI)
    static constexpr int TRACK_CTRL_WIDTH_MINIMAL = 120;
    static constexpr int TRACK_CTRL_WIDTH_STANDARD = 450;
    int getTrackControlWidth() const { return trackControlWidth_; }
    void setTrackControlWidth( int width );
    void saveTrackControlWidth();
    void loadTrackControlWidth();

private:
    int trackControlWidth_ = 120;  // Current width in pixels
    bool trackHeaderDragActive_ = false;
    int trackHeaderDragStartX_ = 0;
    int trackHeaderDragStartWidth_ = 0;
    void appendRowsFor( SObject *container, int depth );
    // Resolve a drag drop at control-column y: *onto = the lane's track if the
    // pointer is over a lane's middle (nest), else NULL; *topSlot = insertion
    // index among top-level tracks (reorder / pop-to-top).
    void resolveDrop( int y, STrack **onto, int *topSlot ) const;

    // Track-reorder drag state. dragControl_ is the control being dragged (NULL
    // when not dragging); dropIndicator_ is a thin line marking the insertion
    // slot. Helper insertSlotAt() maps a Y to a 0..n insertion gap.
    SSMVMixerControl *dragControl_;
    QWidget *dropIndicator_;
    int insertSlotAt( int yInControlBox ) const;

    // Pivot for Shift-click range selection: the last head clicked WITHOUT
    // Shift. Kept here rather than on the model because it is a gesture
    // detail — the model only knows the resulting set and its primary.
    QPointer<STrack> selectionAnchor_;

    // One structural step of the grouping gestures, applied to a single track
    // and re-resolving its path first (each step shifts the indices the next
    // one would have used). Return false when the step does not apply.
    bool indentOne_( STrack *t, const QList<STrack *> &alsoMoving );
    bool outdentOne_( STrack *t );
    void ungroupOne_( STrack *t );
};


#endif


