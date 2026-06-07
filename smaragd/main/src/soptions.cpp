#include "soptions.h"

QVariant SOpt::def( const QString &key )
{
    // Scroll-first defaults.
    if( key == WheelPlain )     return (int) ScrollVertical;
    if( key == WheelShift )     return (int) ScrollHorizontal;
    if( key == WheelCtrl )      return (int) ZoomHorizontal;
    if( key == WheelCtrlShift ) return (int) ZoomVertical;
    if( key == ZoomToCursor )   return true;
    if( key == InvertZoom )     return false;
    if( key == AudioDeviceId )  return QString();
    return QVariant();
}

QString SOpt::wheelActionLabel( WheelAction a )
{
    switch( a ) {
    case None:             return "Do nothing";
    case ScrollVertical:   return "Scroll tracks (vertical)";
    case ScrollHorizontal: return "Scroll timeline (horizontal)";
    case ZoomHorizontal:   return "Zoom horizontal";
    case ZoomVertical:     return "Zoom vertical";
    }
    return "Do nothing";
}
