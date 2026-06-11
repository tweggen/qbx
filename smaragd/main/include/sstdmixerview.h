
#ifndef _SSTDMIXERVIEW_H
#define _SSTDMIXERVIEW_H

#include <qwidget.h>
#include <sobjectrenderer.h>
//#include <qptrvector.h>
#include <qtoolbutton.h>
#include <QVector>
#include <QSet>
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
struct STrackRow {
    STrack  *track;        // the track shown on this lane
    SLink   *link;         // the SLink wrapping it in its parent (timeline pos)
    SObject *parent;       // the container that holds `link` (mixer or a track)
    int      depth;        // 0 = top-level (mixer child)
    bool     hasChildren;  // has at least one child *track* (is foldable)
    bool     collapsed;    // children hidden
};

class SMVActualView 
    : public QWidget
{
    Q_OBJECT
public:
    SMVActualView( QWidget *parent, SStdMixerView & );
    virtual ~SMVActualView();

    double getSecondWidth() const { return secondWidth_; }
    int getTrackHeight() const { return trackHeight_; }
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

protected:
    virtual void paintEvent( QPaintEvent * );
    virtual void mousePressEvent( QMouseEvent * );
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

private:
    void updateLastClickVars( const QPoint & );

    // Mouse-wheel navigation config, cached from SSettings (SOpt keys) and
    // refreshed when settings change. wheelActionFor() maps a modifier combo to
    // an SOpt::WheelAction. See wheelEvent().
    void loadWheelConfig();
    int  wheelActionFor( Qt::KeyboardModifiers mods ) const;
    int  wheelPlain_, wheelShift_, wheelCtrl_, wheelCtrlShift_;
    bool wheelZoomToCursor_, wheelInvertZoom_;

    // --- time-range selection -------------------------------------------
    enum RangeDrag { RangeNone, RangeCreate, RangeMoveStart, RangeMoveEnd };
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
    idx_t upperLeftY_;
    offset_t upperLeftOffset_;

    int trackHeight_;
    double secondWidth_;
    QMenu *qGlobalPopup_;
    QMenu *qRangePopup_;
    QAction *qRangeActClear_;
    bool rangeValid_;
    offset_t rangeStart_, rangeEnd_;   // the two ends (not necessarily ordered)
    int rangeDrag_;                    // RangeDrag
    int lastClickTrackIdx_;
    STrack *lastClickTrack_;
    SLink *lastClickSLink_;
    offset_t lastClickOffset_;
    QPoint lastClickPos_;
    offset_t lastClickSelStartOffset_;
    bool lastClickedStart_, lastClickedEnd_;
    bool lastClickedEndUpper_ = false;   // right edge band AND cursor in upper lane half
    length_t lastClickDuration_;

    // Clip MOVE drag snapshot: captured at press so the move lands as one
    // undoable SMoveClipAction on release (the drag itself mutates live).
    bool     clipDragArmed_ = false;
    STrack  *clipDragTrack0_ = nullptr;
    offset_t clipDragStart0_ = 0;
    // SCut start-offset at press, for the resize (edge-drag) undo snapshot.
    offset_t clipResizeOffset0_ = 0;
    // Loop length / grain stretch at press, for the loop & time-stretch gestures.
    length_t clipLoopLen0_ = 0;   // original loop length (revert snapshot)
    length_t clipLoopSeg_  = 0;   // loop segment captured during a loop drag
    double   clipStretch0_ = 1.0;
    // Which clip-edit gesture this drag is: Alt-body = slip, Ctrl-border = stretch,
    // right-upper edge = loop. (Resize/extend/trim use lastClickedStart_/End_.)
    bool clipDragIsSlip_ = false;
    bool clipDragIsStretch_ = false;
    bool clipDragIsLoop_ = false;
    void updateHoverCursor( const QPoint &pos );   // telegraph the gesture on hover

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

    offset_t alignTime( offset_t );

    int getTrackHeight() const;

    // --- flattened track-tree (depth-first) the arranger draws -----------
    // The view shows one lane per STrackRow. rows_ is rebuilt by refreshTrackTree
    // whenever the tree or a fold state changes; everything that used to index
    // the flat mixer (paint, hit-testing, drag, scrolling, the control column)
    // now indexes these rows.
    int rowCount() const { return rows_.size(); }
    const STrackRow *rowAt( int i ) const;
    int rowIndexOfTrack( const STrack * ) const;
    bool isTrackCollapsed( STrack *t ) const { return collapsed_.contains( t ); }
    void toggleTrackCollapsed( STrack * );
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

protected slots:
    void viewResized();

private slots:
    void contentDurationChanged( length_t newDuration );
    void nTracksChanged();
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

        GLCOLSTRETCH_0 = 0,
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

    bool snapToTimeGrid_;
    STimeGridSpec timeGridSpec_;
    SSnapSpec *currentSnapSpec_;

    QWidget *qTrackControlBoxHolder_;
    QWidget *qTrackControlBox_;
    QVector<SSMVMixerControl*> *controlArray_;

    // Flattened track tree + per-track fold state (UI-only).
    QVector<STrackRow> rows_;
    QSet<STrack*> collapsed_;
    void rebuildRows();
    void rebuildControlColumn();
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


