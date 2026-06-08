
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
#include <QApplication>
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
    return QSize( SMV_TRACK_CTRL_WIDTH, smv_.getTrackHeight() );
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
        ev->accept();
        // Toggling rebuilds the control column (deletes controls, incl. this
        // one) — do it after this event returns, never inside the handler.
        SStdMixerView *view = &smv_;
        STrack *tk = &tk_;
        QMetaObject::invokeMethod( view, [view, tk]() { view->toggleTrackCollapsed( tk ); },
                                   Qt::QueuedConnection );
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
        dragging_ = false;
        dragArmed_ = false;
        unsetCursor();
        update();
        int y = mapToParent( ev->pos() ).y();
        ev->accept();
        // endTrackDrag can rebuild the control column (deleting controls,
        // including this one). Run it after this event returns so we never
        // mutate/destroy widgets from inside their own event dispatch.
        SStdMixerView *view = &smv_;
        QMetaObject::invokeMethod( view, [view, y]() { view->endTrackDrag( y ); },
                                   Qt::QueuedConnection );
        return;
    }
    dragArmed_ = false;
    QWidget::mouseReleaseEvent( ev );
}

void SSMVMixerControl::muteToggled( bool on )
{
    tk_.setMuted( on );
    // Mute/solo changes the rendered output, so drop cached renders (asset
    // captures) — they are not actions and so don't hit the action chokepoint.
    if( SProject *p = SApplication::app().getCurrentProject() )
        p->notifyArrangementChanged();
}

void SSMVMixerControl::soloToggled( bool on )
{
    tk_.setSolo( on );
    if( SProject *p = SApplication::app().getCurrentProject() )
        p->notifyArrangementChanged();
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

    // Small label font derived from the application default (the bundled
    // FreeSans, antialiased) so the channel strip matches the rest of the UI.
    QFont smallFont = QApplication::font();
    smallFont.setPointSize( 7 );

    qTrkLabel_ = new QLineEdit( tk_.getSName(), this );
    qTrkLabel_->setFrame( false );
    qTrkLabel_->setFont( smallFont );

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
    qVolLabel_->setFont( smallFont );

    // Small square Mute / Solo toggle buttons. Mute (red when on) silences this
    // track; Solo (yellow when on) silences every track that is not soloed.
    // Small square Mute / Solo toggle buttons. A compact bold font keeps the
    // single glyph from being clipped inside the 20x20 button (the default
    // FreeSans size was too tall) while preserving the native button look.
    QFont btnFont = QApplication::font();
    btnFont.setPointSize( 8 );
    btnFont.setBold( true );
    qMute_ = new QPushButton( "M", this );
    qMute_->setCheckable( true );
    qMute_->setFixedSize( 20, 20 );
    qMute_->setFont( btnFont );
    qMute_->setToolTip( "Mute" );
    qMute_->setStyleSheet( "QPushButton:checked { background:#d04040; color:white; }" );
    qSolo_ = new QPushButton( "S", this );
    qSolo_->setCheckable( true );
    qSolo_->setFixedSize( 20, 20 );
    qSolo_->setFont( btnFont );
    qSolo_->setToolTip( "Solo" );
    qSolo_->setStyleSheet( "QPushButton:checked { background:#e0c020; color:black; }" );

    // Mute over Solo in a column.
    QVBoxLayout *muteSoloCol = new QVBoxLayout();
    muteSoloCol->setContentsMargins( 0, 0, 0, 0 );
    muteSoloCol->setSpacing( 2 );
    muteSoloCol->addWidget( qMute_, 0, Qt::AlignTop );
    muteSoloCol->addWidget( qSolo_, 0, Qt::AlignTop );
    muteSoloCol->addStretch( 1 );

    // Fader with its dB readout directly beneath it, both centred so they line
    // up as one column.
    QVBoxLayout *faderCol = new QVBoxLayout();
    faderCol->setContentsMargins( 0, 0, 0, 0 );
    faderCol->setSpacing( 1 );
    faderCol->addWidget( qVolume_, 1, Qt::AlignHCenter );
    faderCol->addWidget( qVolLabel_, 0, Qt::AlignHCenter );

    // Mute/Solo column, then the fader column; a trailing stretch keeps the
    // group left-aligned.
    QHBoxLayout *stripRow = new QHBoxLayout();
    stripRow->setContentsMargins( 0, 0, 0, 0 );
    stripRow->setSpacing( 4 );
    stripRow->addLayout( muteSoloCol );
    stripRow->addLayout( faderCol );
    stripRow->addStretch( 1 );

    setFixedSize( SMV_TRACK_CTRL_WIDTH, smv_.getTrackHeight() );

    qLayout_->addWidget( qTrkLabel_, 0, 0, Qt::AlignTop );
    qLayout_->addLayout( stripRow,   1, 0 );
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
