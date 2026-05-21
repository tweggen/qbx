
#include <qpainter.h>

#include "tw303aenv.h"
#include "twcomponent.h"

#include "sapplication.h"
#include "sobject.h"
#include "slink.h"
#include "splainwave.h"
#include "splainwaverndrinline.h"

void SPlainWaveRendererInline::draw( SLink &lk, SRenderContext &ctx )
{
    QPainter &p = ctx.getPainter();
    QRect r = ctx.getVisibRect();
    int tl = r.topLeft().x();
    int top = r.topLeft().y();
    int height = r.height();
    offset_t o1 = ctx.getTimeOf( r.topLeft().x() );
    offset_t o2 = ctx.getTimeOf( r.bottomRight().x() );    
    if( o2<=o1 ) o2 = o1+1;
    o1 -= lk.getStartTime();
    o2 -= lk.getStartTime();
    int w = r.width();
    if( w<1 ) w=1;
    preview_t *pv = (preview_t *) alloca( sizeof( preview_t )* w );
    // We know that we have a preview.
//    qWarning( "plainWaveRender: o1 = %d, o2 = %d, width = %d.\n",
//              (int)o1, (int)o2, w );
    int res = getPlainWave().getPreview( pv, o1, o2-o1, w );
    if( res<0 ) {
        p.fillRect( r, QColor( 160, 128, 128 ) );
        p.setPen( QColor( 160, 30, 30 ) );
        p.drawText( r, Qt::AlignCenter, "No Preview" );
        return;
    } 
//    bool isSel = SApplication::app().isSLinkSelected( &lk );
//    if( isSel ) {
        p.setPen( QColor( 240, 240, 10 ) );
//     } else {
//        p.setPen( QColor( 200, 200, 200 ) );
//    }
    for( int i=0; i<w; i++ ) {
        int x = i+tl;
        int y1 = top+((127-pv[i].min)*height)/256;
        int y2 = top+((127-pv[i].max)*height)/256;
#if 0
        if( y2<y1 ) {
            int h = y2;
            y2 = y1;
            y1 = h;
        }
#endif        
        p.drawLine( x, y1, x, y2 );
    }
//    if( isSel ) 
    p.setPen( QColor( 10, 10, 40 ) );
//    else p.setPen( QColor( 0, 0, 0 ) );
    p.drawText( r, Qt::AlignBottom|Qt::AlignRight, getPlainWave().getFileName() );
}
