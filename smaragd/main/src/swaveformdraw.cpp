#include "swaveformdraw.h"

#include <qpainter.h>
#include <QVarLengthArray>

#include "sobject.h"
#include "slink.h"
#include "sobjectrenderer.h"

bool drawObjectWaveform( SObject &obj, SLink &lk, SRenderContext &ctx,
                         const QColor &waveColor )
{
    QPainter &p = ctx.getPainter();
    QRect r = ctx.getVisibRect();
    int tl     = r.topLeft().x();
    int top    = r.topLeft().y();
    int height = r.height();

    // Time span of the visible rect, in the object's own (source) domain: the
    // context maps pixel -> time, and we subtract the link start so 0 is the
    // object's origin (the cut context folds its window offset in here).
    offset_t o1 = ctx.getTimeOf( r.topLeft().x() );
    offset_t o2 = ctx.getTimeOf( r.bottomRight().x() );
    if( o2 <= o1 ) o2 = o1 + 1;
    o1 -= lk.getStartTime();
    o2 -= lk.getStartTime();

    int w = r.width();
    if( w < 1 ) w = 1;

    QVarLengthArray<preview_t> pv( w );
    int res = obj.getPreview( pv.data(), o1, o2 - o1, w );
    if( res < 0 ) return false;

    p.setPen( waveColor );
    for( int i = 0; i < w; i++ ) {
        int x  = i + tl;
        int y1 = top + ( ( 127 - pv[i].min ) * height ) / 256;
        int y2 = top + ( ( 127 - pv[i].max ) * height ) / 256;
        p.drawLine( x, y1, x, y2 );
    }
    return true;
}
