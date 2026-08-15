#include "app/eventui/seventtimeaxis.h"

SEventTimeAxis::SEventTimeAxis( QObject *parent )
    : QObject( parent )
{
}

int SEventTimeAxis::xOfFrame( offset_t frame ) const
{
    // Byte-for-byte the arranger's formula (SMVActualView::getXPosOfOffset).
    return ( (int) ( ( ( (double) frame ) / sampleRate_ ) * secondWidth_ ) )
           - leftPixels_;
}

offset_t SEventTimeAxis::frameOfX( int x ) const
{
    // ... and of SMVActualView::getTimeOf.
    const double a = ( (double) ( x + leftPixels_ ) ) / secondWidth_ * sampleRate_;
    return a < 0.0 ? (offset_t) 0 : (offset_t) a;
}

void SEventTimeAxis::setSecondWidth( double pixelsPerSecond )
{
    if( pixelsPerSecond < 0.000001 ) pixelsPerSecond = 0.000001;
    if( secondWidth_ == pixelsPerSecond ) return;
    secondWidth_ = pixelsPerSecond;
    emit axisChanged();
}

void SEventTimeAxis::setLeftPixels( int px )
{
    if( px < 0 ) px = 0;
    if( leftPixels_ == px ) return;
    leftPixels_ = px;
    emit axisChanged();
}

void SEventTimeAxis::setLeftFrame( offset_t frame )
{
    setLeftPixels( (int) ( ( ( (double) frame ) / sampleRate_ ) * secondWidth_ ) );
}

void SEventTimeAxis::setSampleRate( int srate )
{
    if( srate <= 0 || sampleRate_ == srate ) return;
    sampleRate_ = srate;
    emit axisChanged();
}

void SEventTimeAxis::setLinked( bool linked )
{
    if( linked_ == linked ) return;
    linked_ = linked;
    emit axisChanged();
}
