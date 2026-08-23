
#include <qpainter.h>
#include <QFontMetrics>

#include "tw/graph/tw303aenv.h"
#include "tw/graph/twcomponent.h"

#include "app/model/sobject.h"
#include "app/model/slink.h"
#include "app/objects/wave/splainwave.h"
#include "app/objects/wave/splainwaverndrinline.h"
#include "app/objects/wave/swaveformdraw.h"

namespace {
// Proposal 27 M1: a corner "analyzing…" badge, shown while this wave's
// background sidecar analysis (onsets, loudness) is queued or running. Cloned
// from SCutRendererInline::drawPitchBadge — QFontMetrics-sized box anchored
// top-left (+2,+2), self-contained here, bails when the clip is too narrow to
// hold it. Amber text over a translucent dark rounded fill.
void drawAnalyzingBadge( QPainter &p, const QRect &visibRect )
{
    const QString text = QStringLiteral( "analyzing…" );

    QFontMetrics fm( p.font() );
    QRect box = fm.boundingRect( text ).adjusted( -3, -1, 3, 1 );
    box.moveTopLeft( visibRect.topLeft() + QPoint( 2, 2 ) );
    if( !visibRect.contains( box ) ) return;   // clip too narrow for the badge

    p.save();
    p.setPen( Qt::NoPen );
    p.setBrush( QColor( 20, 20, 30, 180 ) );
    p.drawRoundedRect( box, 3, 3 );
    p.setPen( QColor( 255, 200, 60 ) );
    p.drawText( box, Qt::AlignCenter, text );
    p.restore();
}
}

void SPlainWaveRendererInline::draw( SLink &lk, SRenderContext &ctx )
{
    QPainter &p = ctx.getPainter();
    QRect r = ctx.getVisibRect();

    // The waveform colour is THE TRACK'S (app/model/sclipcolors.h), handed
    // down on the context by whoever filled the clip body. It was a fixed
    // yellow, which said "audio sample" on every track in every project.
    if( !drawObjectWaveform( getPlainWave(), lk, ctx, ctx.clipColors().wave ) ) {
        p.fillRect( r, QColor( 160, 128, 128 ) );
        p.setPen( QColor( 160, 30, 30 ) );
        p.drawText( r, Qt::AlignCenter, "No Preview" );
        return;
    }
    p.setPen( QColor( 10, 10, 40 ) );
    p.drawText( r, Qt::AlignBottom|Qt::AlignRight, getPlainWave().getFileName() );

    if( getPlainWave().isAnalyzing() )
        drawAnalyzingBadge( p, r );
}

// The COLLECT terminal (proposal 39 M1). A plain wave has no domain mapping of
// its own beyond the link start, which collectObjectEnvelope folds, so this is
// the same single call the draw path makes.
bool SPlainWaveRendererInline::collectEnvelope( SLink &lk,
                                                const SEnvelopeWindow &win,
                                                preview_t *out )
{
    return collectObjectEnvelope( getPlainWave(), lk, win, out );
}
