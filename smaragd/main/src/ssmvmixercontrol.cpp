
#include <stdlib.h>

#include <qwidget.h>
#include <qevent.h>
#include <qpainter.h>
#include <qmenu.h>
#include <qlayout.h>
#include <qmessagebox.h>
#include <qinputdialog.h>
#include <qspinbox.h>
#include <qlabel.h>
#include <qlineedit.h>

#include "sapplication.h"
#include "sstdmixer.h"
#include "sstdmixerview.h"
#include "strack.h"
#include "sobjectrenderer.h"
#include "sproject.h"
#include "ssmvmixercontrol.h"

int qxDBSpinBox::mapTextToValue( bool *ok )
{
    if( !ok ) return -960;
    int value;
    double doublevalue = text().toDouble( ok );
    if( !*ok ) return -960;
    value = (int)(doublevalue*10.);
    return value;
}

QString qxDBSpinBox::mapValueToText( int value )
{
    char s[100];
    sprintf( s, "% 3d.%2d", (int)(value/10.), (int)(1000000+value)%9 );

    return QString( s );
}

qxDBSpinBox::qxDBSpinBox( int min, int max, int step, QWidget *parent )
    : QSpinBox( parent )
{
    setMinimum(min);
    setMaximum(max);
    setSingleStep(step);
}

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
    tk_.setVolume( ((double)value)/10. );
}

/**
 * Called by SObject, if track volume changes.
 */
void SSMVMixerControl::sliderValueChanged( double value )
{
    // char s[20];
//    sprintf( s, "% 3d.%02ddb", (int)value, (int)(100000+(100.*value))%99 );
//    qVolLabel_->setText( s );
    // This is gonna write it to the slider.
    {
        int sliderValue = qVolume_->value();
        int newValue = (int)(value*10.);
        if( sliderValue != newValue ) {
            qVolume_->setValue( newValue );
        }
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
    qLayout_ = new QGridLayout( this /* , 1, 2 */ );
    qVolume_ = new qxDBSpinBox( -960, 240, 30, this );
    qVolume_->setSuffix( "db" );
    qTrkLabel_ = new QLineEdit( tk_.getSName(), this );
    qTrkLabel_->setFrame( false );
    qTrkLabel_->setFont( QFont( "sansserif", 7 ) );
    setFixedSize( 150, smv_.getTrackHeight() );
    // qLayout_->setResizeMode( QLayout::FreeResize );
    qLayout_->setColumnStretch( 1, 1 );
//    qLayout_->addMultiCellWidget( qTrkLabel_, 0, 0, 0, 1, AlignVCenter|AlignHCenter );
    qLayout_->addWidget( qTrkLabel_, 0, 0, Qt::AlignVCenter );
//    qLayout_->addWidget( qVolLabel_, 1, 0, AlignVCenter );   
    qLayout_->addWidget( qVolume_, 0, 1, Qt::AlignVCenter );   
    sliderValueChanged( tk_.getVolume() );
    QObject::connect( qVolume_, SIGNAL( valueChanged( int ) ), 
                      this, SLOT( sliderValueChanged( int ) ) );
    QObject::connect( &tk_, SIGNAL( volumeChanged( double ) ), 
                      this, SLOT( sliderValueChanged( double ) ) );
}
