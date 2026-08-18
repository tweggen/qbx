#include <QPainter>

#include "app/model/slink.h"
#include "app/objects/wave/srecordingcontent.h"
#include "app/objects/wave/srecordingrndrinline.h"
#include "app/objects/wave/swaveformdraw.h"

void SRecordingRendererInline::draw( SLink &lk, SRenderContext &ctx )
{
    QPainter &p = ctx.getPainter();
    const QRect r = ctx.getVisibRect();

    SRecordingContent &rec = getRecording();

    // The waveform, in the same red the transport uses for RECORD.
    if( !drawObjectWaveform( rec, lk, ctx, QColor( 255, 120, 120 ) ) ) {
        p.fillRect( r, QColor( 90, 40, 40 ) );
    }

    // THE FRONTIER. getTimeOf() is the pixel -> clip-relative-frame map the
    // context already supplies for the waveform, so inverting it by search is
    // unnecessary: walk the visible columns and stop at the first pixel whose
    // time is at or past the published length. One pass, no geometry
    // assumptions, correct under the loop tiling too.
    const offset_t len = rec.getDuration();
    int fx = -1;
    for( int x = r.x(); x < r.x() + r.width(); ++x ) {
        if( ctx.getTimeOf( x ) >= len ) { fx = x; break; }
    }
    if( fx >= 0 ) {
        p.setPen( QPen( QColor( 255, 60, 60 ), 2 ) );
        p.drawLine( fx, r.y(), fx, r.y() + r.height() );
    }

    if( !rec.isFrozen() ) {
        p.setPen( QColor( 255, 200, 200 ) );
        p.drawText( r, Qt::AlignBottom | Qt::AlignLeft, QStringLiteral( " REC" ) );
    }
}

// The COLLECT terminal (proposal 39 M1): the same shared envelope the draw
// loop walks, out of the content's own incrementally-extended peak ladder.
bool SRecordingRendererInline::collectEnvelope( SLink &lk,
                                                const SEnvelopeWindow &win,
                                                preview_t *out )
{
    return collectObjectEnvelope( getRecording(), lk, win, out );
}
