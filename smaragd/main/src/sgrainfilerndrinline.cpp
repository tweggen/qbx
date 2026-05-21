
#include <qpainter.h>

#include "tw303aenv.h"
#include "twcomponent.h"

#include "sapplication.h"
#include "sobject.h"
#include "slink.h"
#include "sgrainfile.h"
#include "sgrainfilerndrinline.h"

void SGrainFileRendererInline::draw( SLink &/*lk*/, SRenderContext &ctx )
{
    QPainter &p = ctx.getPainter();
    QRect r = ctx.getVisibRect();

    p.fillRect( r, QColor( 160, 128, 128 ) );
    p.setPen( QColor( 160, 30, 30 ) );
    p.drawText( r, Qt::AlignCenter, "Grained object. Np preview function coded yet." );
}
