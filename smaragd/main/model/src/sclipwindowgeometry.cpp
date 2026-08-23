#include "app/model/sclipwindowgeometry.h"

#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>

QRect sClipWindowLoopHandleRect( const QRect &clipRect, int x )
{
    QFont f = QGuiApplication::font();
    f.setPointSize( 7 );
    int h = QFontMetrics( f ).height();
    int maxH = clipRect.height() - 2;
    if( h > maxH ) h = maxH;
    if( h < 4 ) return QRect();                  // lane too short for a grip
    return QRect( x - SCLIPWIN_LOOP_HANDLE_W/2, clipRect.y() + 1,
                  SCLIPWIN_LOOP_HANDLE_W, h );
}
