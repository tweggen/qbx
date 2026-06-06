
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

SSMVMixerControl::~SSMVMixerControl()
{
    // Deletes all widgets by default
}

SSMVMixerControl::SSMVMixerControl( 
    QWidget *parent, SStdMixerView &smv, STrack &tk )
    : QWidget( parent ),
      smv_( smv ),
      tk_( tk )
{
    qLayout_ = new QGridLayout( this );
    qLayout_->setContentsMargins( 4, 2, 4, 2 );
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

    setFixedSize( 150, smv_.getTrackHeight() );

    qLayout_->addWidget( qTrkLabel_, 0, 0, Qt::AlignTop );
    qLayout_->addWidget( qVolume_,   1, 0, Qt::AlignHCenter );
    qLayout_->addWidget( qVolLabel_, 2, 0, Qt::AlignHCenter );
    qLayout_->setRowStretch( 1, 1 );

    // Seed the fader position + dB readout from the current track volume.
    setSliderSilently( tk_.getVolume() );

    QObject::connect( qVolume_, SIGNAL( valueChanged( int ) ),
                      this, SLOT( sliderValueChanged( int ) ) );
    QObject::connect( &tk_, SIGNAL( volumeChanged( double ) ),
                      this, SLOT( sliderValueChanged( double ) ) );
}
