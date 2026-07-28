
#ifndef _SSTDMIXERVIEW_H
#define _SSTDMIXERVIEW_H

#include <qwidget.h>
#include <vector>

#include "tw/core/twfraction.h"
#include "tw/core/twwarpmap.h"
#include "app/model/sobjectrenderer.h"
//#include <qptrvector.h>
#include <qtoolbutton.h>
#include <QVector>
#include <QSet>
#include <QHash>
#include <QList>

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
struct STrackRow {
    STrack  *track;        // the track shown on this lane
    SLink   *link;         // the SLink wrapping it in its parent (timeline pos)
    SObject *parent;       // the container that holds `link` (mixer or a track)
    int      depth;        // 0 = top-level (mixer child)
    bool     hasChildren;  // has at least one child *track* (is foldable)
    bool     collapsed;    // children hidden
    // Take lanes (proposal 17 phase 3): -1 = the track's normal (composite)
    // lane; k >= 0 = the row showing take k of every take stack on the track.
    // Any row with takeRow >= 0 is a SUB-LANE: it hangs off the track lane
    // above it, carries no channel strip of its own, and is covered by that
    // track's head. Automation lanes will join it under the same rule.
    int      takeRow = -1;
    // This lane's pixel height, filled by rebuildRows(). 0 until then.
    int      height = 0;

    // A sub-lane belongs to the track lane above it rather than standing on
    // its own (no head, not a reorder target of its own).
    bool isSubLane() const { return takeRow >= 0; }
};

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
    offset_t getTimeOf( int x ) const;
    idx_t getLastClickTrackIdx() const { return lastClickTrackIdx_; }
    STrack *getLastClickTrack() const { return lastClickTrack_; }
    void resetLastClickTrack() { lastClickTrack_ = NULL; }
    SLink *getLastClickSLink() const { return lastClickSLink_; }
    void resetLastClickSLink() { lastClickSLink_ = NULL; }
    offset_t getLastClickOffset() const { return lastClickOffset_; }
    QPoint getLastClickPos() const { return lastClickPos_; }
    offset_t getLastClickStartOffset() const { return lastClickSelStartOffset_; }
    int getXPosOfOffset( offset_t ) const;
    QRect getSLinkVisibRect( int trackIdx, const SLink & );

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
    void secondWidthChanged( int x );
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

signals:
    void snapMethodChanged( int );
    void beatSubDivChanged( idx_t );

public slots:
    void setBeatSubDiv( idx_t );
    void setSnapMethod( int );
protected:
private:
    idx_t beatSubDiv_;
    int snapMethod_;
    int sampleRate_;
    STimeGridSpec &tgs_;
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
    bool dragClipEdge( int rowIdx, int clipIdx, int grabWhere, offset_t dropTime,
                       bool upperHalf, Qt::KeyboardModifiers mods = Qt::NoModifier );

    // TEST ENTRY POINTS for the lane geometry. The heads are placed by
    // layoutControlColumn() while the lanes are drawn by the canvas; nothing
    // else can prove the two agree, and they only ever disagreed *after* some
    // event moved one of them — hence the knobs to move things first.
    void tkSetBaseTrackHeight( int h );      // vertical zoom, absolute
    void tkSetTopRow( int row );             // vertical scroll, in lanes
    // "" when every head sits exactly on its lane (same top, same height as
    // the painter uses, full column width), otherwise the first mismatch.
    QString tkCheckLaneAlignment() const;

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
    bool isTrackCollapsed( STrack *t ) const { return collapsed_.contains( t ); }
    void toggleTrackCollapsed( STrack * );
    // Take lanes (proposal 17 phase 3): per-track expanded state, UI-only.
    // An expanded track shows one extra row per take index below its lane.
    bool isTrackTakesExpanded( STrack *t ) const
        { return takesExpanded_.contains( t ); }
    void toggleTrackTakesExpanded( STrack * );
    void refreshTrackTree();   // rebuild rows + control column + relayout + repaint
    // --------------------------------------------------------------------

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
    // click in the blank space below the track heads. Routes through the
    // undoable SAction system.
    void ctAddTrackBelowLast();
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
    void setBPMTempo( double );
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
    QSet<STrack*> collapsed_;
    QSet<STrack*> takesExpanded_;   // tracks showing their take lanes
    QHash<const STrack*, double> trackScale_;   // per-track lane height factor
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
};


#endif


