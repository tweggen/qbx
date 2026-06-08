
#include <qpainter.h>

#include "tw303aenv.h"
#include "twcomponent.h"

#include "sapplication.h"
#include "sobject.h"
#include "slink.h"
#include "splainwave.h"
#include "splainwaverndrinline.h"
#include "swaveformdraw.h"

void SPlainWaveRendererInline::draw( SLink &lk, SRenderContext &ctx )
{
    QPainter &p = ctx.getPainter();
    QRect r = ctx.getVisibRect();

    if( !drawObjectWaveform( getPlainWave(), lk, ctx, QColor( 240, 240, 10 ) ) ) {
        p.fillRect( r, QColor( 160, 128, 128 ) );
        p.setPen( QColor( 160, 30, 30 ) );
        p.drawText( r, Qt::AlignCenter, "No Preview" );
        return;
    }
    p.setPen( QColor( 10, 10, 40 ) );
    p.drawText( r, Qt::AlignBottom|Qt::AlignRight, getPlainWave().getFileName() );
}
