
#ifndef _SSTDMIXERVIEW_H
#define _SSTDMIXERVIEW_H

#include <qwidget.h>
#include <sobjectrenderer.h>
//#include <qptrvector.h>
#include <qtoolbutton.h>

class SStdMixer;
class QGridLayout;
class QPaintEvent;
class QMouseEvent;
class QPopupMenu;
class QScrollBar;
class STrack;
class SStdMixerView;
class SLink;
class QPushButton;
class SSMVMixerControl;

#define SMV_CUT_MIN_TIME 1024
#define SMV_LEFT_DRAG_PIXEL 7
#define SMV_RIGHT_DRAG_PIXEL 7

#define SMV_TIME_RULER_HEIGHT 16

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

protected:
    virtual void paintEvent( QPaintEvent * );
    virtual void mousePressEvent( QMouseEvent * );
    virtual void mouseMoveEvent( QMouseEvent * );
    virtual void mouseReleaseEvent( QMouseEvent * );
    virtual void contextMenuEvent( QContextMenuEvent * );
    virtual void resizeEvent( QResizeEvent * );
private slots:
    void globalLocatorMoved( offset_t, offset_t );

private:
    void updateLastClickVars( const QPoint & );
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
    int lastClickTrackIdx_;
    STrack *lastClickTrack_;
    SLink *lastClickSLink_;
    offset_t lastClickOffset_;
    QPoint lastClickPos_;
    offset_t lastClickSelStartOffset_;
    bool lastClickedStart_, lastClickedEnd_;
    length_t lastClickDuration_;
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

public slots:
    void ctAddTrack();
    void ctRemoveTrack();
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

    bool snapToTimeGrid_;
    STimeGridSpec timeGridSpec_;
    SSnapSpec *currentSnapSpec_;

    QWidget *qTrackControlBoxHolder_;
    QWidget *qTrackControlBox_;
    QVector<SSMVMixerControl*> *controlArray_;
};


#endif


