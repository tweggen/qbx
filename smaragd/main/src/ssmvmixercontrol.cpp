
#include <stdlib.h>

#include <qwidget.h>
#include <qevent.h>
#include <qpainter.h>
#include <qmenu.h>
#include <qlayout.h>
#include <qmessagebox.h>
#include <qinputdialog.h>
#include <qslider.h>
#include <qlabel.h>
#include <qlineedit.h>
#include <qpushbutton.h>
#include <QPolygon>
#include <QSignalBlocker>

#include "sapplication.h"
#include "sstdmixer.h"
#include "sstdmixerview.h"
#include "strack.h"
#include "slink.h"
#include "sobjectrenderer.h"
#include "sproject.h"
#include "ssmvmixercontrol.h"
#include "actions/ssettrackvolumeaction.h"

// The fader works in tenths of a dB so it can use an integer QSlider:
// slider value v  <->  v/10 dB. Range -96.0 .. +24.0 dB.
static const int FADER_MIN = -960;
static const int FADER_MAX =  240;

// Width of the grip strip down the left side of the control that acts as the
// track-reorder drag handle, and the pixels the pointer must travel before a
// press there turns into a drag.
static const int HANDLE_W = 12;
static const int DRAG_THRESHOLD = 4;

QSize SSMVMixerControl::sizeHint() const
{
//    printf( "Returning a size of 150/%d.\n",smv_.getTrackHeight() );
    return QSize( 150, smv_.getTrackHeight() );
}

/**
 * Called from QSlider
 */
void SSMVMixerControl::sliderValueChanged( int value )
{
    double newVolume = ((double)value)/10.;

    // Route the change through the action system so it is undoable and (once the
    // engine drain goes async) coalescable. Resolve our track's index in the
    // mixer; if we can't, fall back to a direct mutation so the UI never wedges.
    int trackIdx = trackIndex_();
    if( trackIdx >= 0 ) {
        SApplication::app().submitAction(
            new SSetTrackVolumeAction( trackIdx, newVolume ) );
    } else {
        tk_.setVolume( newVolume );
    }
}

/**
 * Locate this control's track within the mixer model. Returns -1 if not found
 * (e.g. the track was removed). The action layer keys off this index.
 */
int SSMVMixerControl::trackIndex_() const
{
    SStdMixer *mixer = smv_.getModel();
    if( !mixer ) return -1;

    int n = mixer->getNTracks();
    for( int i = 0; i < n; ++i ) {
        SLink *link = mixer->getTrackAt( i );
        if( link && &link->getSObject() == &tk_ ) {
            return i;
        }
    }
    return -1;
}

/**
 * Called by SObject, if track volume changes (model -> view).
 */
void SSMVMixerControl::sliderValueChanged( double value )
{
    setSliderSilently( value );
}

/**
 * Move the fader to reflect a model-side volume change WITHOUT emitting
 * valueChanged() (which would submit a redundant action and corrupt the undo
 * stack during an undo). Always refreshes the dB readout.
 */
void SSMVMixerControl::setSliderSilently( double value )
{
    int newValue = (int)(value*10.);
    if( qVolume_->value() != newValue ) {
        QSignalBlocker block( qVolume_ );
        qVolume_->setValue( newValue );
    }
    if( qVolLabel_ ) {
        qVolLabel_->setText( QString::asprintf( "%+.1f dB", value ) );
    }
}

void SSMVMixerControl::setTreeInfo( int depth, bool foldable, bool collapsed )
{
    depth_ = depth;
    foldable_ = foldable;
    collapsed_ = collapsed;
    // Indent the content: [depth indent][fold gutter][grip][content].
    int left = depth_*SMV_TRACK_INDENT + SMV_FOLD_W + HANDLE_W;
    qLayout_->setContentsMargins( left, 2, 4, 2 );
    update();
}

// The grip's left edge, accounting for indent + fold gutter.
static inline int gripLeft( int depth ) { return depth*SMV_TRACK_INDENT + SMV_FOLD_W; }

// Draw the indented grip strip and, for parents, a fold triangle to its left.
void SSMVMixerControl::paintEvent( QPaintEvent *ev )
{
    QWidget::paintEvent( ev );
    QPainter p( this );

    int gx = gripLeft( depth_ );
    QRect handle( gx, 0, HANDLE_W, height() );
    p.fillRect( handle, dragging_ ? QColor( 40, 90, 160 ) : QColor( 70, 70, 80 ) );
    p.setPen( QColor( 165, 165, 175 ) );
    int cx = gx + HANDLE_W/2;
    int cy = height()/2;
    for( int i=-3; i<=3; ++i ) {
        p.drawPoint( cx-2, cy + i*6 );
        p.drawPoint( cx+1, cy + i*6 );
    }

    if( foldable_ ) {
        int fx = depth_*SMV_TRACK_INDENT;
        int midY = SMV_FOLD_W/2 + 2;
        p.setPen( QColor( 60, 60, 60 ) );
        p.setBrush( QColor( 60, 60, 60 ) );
        QPolygon tri;
        if( collapsed_ ) {                      // ▸ collapsed
            tri << QPoint( fx+3, midY-4 ) << QPoint( fx+3, midY+4 ) << QPoint( fx+9, midY );
        } else {                                // ▾ expanded
            tri << QPoint( fx+2, midY-3 ) << QPoint( fx+10, midY-3 ) << QPoint( fx+6, midY+3 );
        }
        p.drawPolygon( tri );
    }
}

void SSMVMixerControl::mousePressEvent( QMouseEvent *ev )
{
    int x = ev->pos().x();
    int fx = depth_*SMV_TRACK_INDENT;
    int gx = gripLeft( depth_ );

    // A left click on the fold triangle toggles this parent's children.
    if( ev->button()==Qt::LeftButton && foldable_ && x>=fx && x<fx+SMV_FOLD_W ) {
        smv_.toggleTrackCollapsed( &tk_ );
        ev->accept();
        return;
    }
    // A left press on the grip strip arms a track-reorder drag.
    if( ev->button()==Qt::LeftButton && x>=gx && x<gx+HANDLE_W ) {
        dragArmed_ = true;
        dragging_ = false;
        dragPressPos_ = ev->pos();
        ev->accept();
        return;
    }
    QWidget::mousePressEvent( ev );
}

void SSMVMixerControl::mouseMoveEvent( QMouseEvent *ev )
{
    if( dragArmed_ && (ev->buttons() & Qt::LeftButton) ) {
        if( !dragging_ ) {
            if( abs( ev->pos().y()-dragPressPos_.y() ) < DRAG_THRESHOLD ) return;
            dragging_ = true;
            setCursor( Qt::ClosedHandCursor );
            smv_.beginTrackDrag( this );
            update();   // repaint the grip in its "active" colour
        }
        // Report the pointer in the control-column's coordinate space.
        smv_.updateTrackDrag( mapToParent( ev->pos() ).y() );
        ev->accept();
        return;
    }
    QWidget::mouseMoveEvent( ev );
}

void SSMVMixerControl::mouseReleaseEvent( QMouseEvent *ev )
{
    if( dragging_ ) {
        smv_.endTrackDrag( mapToParent( ev->pos() ).y() );
        dragging_ = false;
        dragArmed_ = false;
        unsetCursor();
        update();
        ev->accept();
        return;
    }
    dragArmed_ = false;
    QWidget::mouseReleaseEvent( ev );
}

void SSMVMixerControl::muteToggled( bool on )
{
    tk_.setMuted( on );
}

void SSMVMixerControl::soloToggled( bool on )
{
    tk_.setSolo( on );
}

void SSMVMixerControl::onMutedChanged( bool on )
{
    QSignalBlocker block( qMute_ );   // don't bounce back into muteToggled
    qMute_->setChecked( on );
}

void SSMVMixerControl::onSoloChanged( bool on )
{
    QSignalBlocker block( qSolo_ );
    qSolo_->setChecked( on );
}

SSMVMixerControl::~SSMVMixerControl()
{
    // Deletes all widgets by default
}

SSMVMixerControl::SSMVMixerControl(
    QWidget *parent, SStdMixerView &smv, STrack &tk )
    : QWidget( parent ),
      dragArmed_( false ),
      dragging_( false ),
      smv_( smv ),
      tk_( tk )
{
    qLayout_ = new QGridLayout( this );
    // Reserve the left edge for [fold triangle][grip]; setTreeInfo() widens this
    // by the indent for nested tracks.
    qLayout_->setContentsMargins( SMV_FOLD_W + HANDLE_W, 2, 4, 2 );
    qLayout_->setSpacing( 2 );

    qTrkLabel_ = new QLineEdit( tk_.getSName(), this );
    qTrkLabel_->setFrame( false );
    qTrkLabel_->setFont( QFont( "sansserif", 7 ) );

    // Vertical fader, like a channel strip on a console. Works in tenths of a
    // dB; loud at the top (Qt vertical sliders put the maximum at the top).
    qVolume_ = new QSlider( Qt::Vertical, this );
    qVolume_->setRange( FADER_MIN, FADER_MAX );
    qVolume_->setSingleStep( 10 );    // 1.0 dB per arrow key / wheel notch
    qVolume_->setPageStep( 60 );      // 6.0 dB per page
    qVolume_->setTickPosition( QSlider::TicksRight );
    qVolume_->setTickInterval( 120 ); // a tick every 12 dB

    qVolLabel_ = new QLabel( this );
    qVolLabel_->setAlignment( Qt::AlignHCenter );
    qVolLabel_->setFont( QFont( "sansserif", 7 ) );

    // Small square Mute / Solo toggle buttons. Mute (red when on) silences this
    // track; Solo (yellow when on) silences every track that is not soloed.
    qMute_ = new QPushButton( "M", this );
    qMute_->setCheckable( true );
    qMute_->setFixedSize( 20, 20 );
    qMute_->setToolTip( "Mute" );
    qMute_->setStyleSheet( "QPushButton:checked { background:#d04040; color:white; }" );
    qSolo_ = new QPushButton( "S", this );
    qSolo_->setCheckable( true );
    qSolo_->setFixedSize( 20, 20 );
    qSolo_->setToolTip( "Solo" );
    qSolo_->setStyleSheet( "QPushButton:checked { background:#e0c020; color:black; }" );

    // Mute over Solo in a column, with the volume fader to their right.
    QVBoxLayout *muteSoloCol = new QVBoxLayout();
    muteSoloCol->setContentsMargins( 0, 0, 0, 0 );
    muteSoloCol->setSpacing( 2 );
    muteSoloCol->addWidget( qMute_, 0, Qt::AlignTop );
    muteSoloCol->addWidget( qSolo_, 0, Qt::AlignTop );
    muteSoloCol->addStretch( 1 );

    QHBoxLayout *stripRow = new QHBoxLayout();
    stripRow->setContentsMargins( 0, 0, 0, 0 );
    stripRow->setSpacing( 2 );
    stripRow->addLayout( muteSoloCol );
    stripRow->addWidget( qVolume_ );
    stripRow->addStretch( 1 );

    setFixedSize( 150, smv_.getTrackHeight() );

    qLayout_->addWidget( qTrkLabel_, 0, 0, Qt::AlignTop );
    qLayout_->addLayout( stripRow,   1, 0 );
    qLayout_->addWidget( qVolLabel_, 2, 0, Qt::AlignHCenter );
    qLayout_->setRowStretch( 1, 1 );

    // Seed widgets from the current track state.
    setSliderSilently( tk_.getVolume() );
    qMute_->setChecked( tk_.isMuted() );
    qSolo_->setChecked( tk_.isSolo() );

    QObject::connect( qVolume_, SIGNAL( valueChanged( int ) ),
                      this, SLOT( sliderValueChanged( int ) ) );
    QObject::connect( &tk_, SIGNAL( volumeChanged( double ) ),
                      this, SLOT( sliderValueChanged( double ) ) );
    QObject::connect( qMute_, SIGNAL( toggled( bool ) ),
                      this, SLOT( muteToggled( bool ) ) );
    QObject::connect( qSolo_, SIGNAL( toggled( bool ) ),
                      this, SLOT( soloToggled( bool ) ) );
    QObject::connect( &tk_, SIGNAL( mutedChanged( bool ) ),
                      this, SLOT( onMutedChanged( bool ) ) );
    QObject::connect( &tk_, SIGNAL( soloChanged( bool ) ),
                      this, SLOT( onSoloChanged( bool ) ) );
}
