#include "app/objects/wave/swaveformdraw.h"

#include <qpainter.h>
#include <QVarLengthArray>
#include <cmath>

#include "app/model/sobject.h"
#include "app/model/slink.h"
#include "app/model/sobjectrenderer.h"

SEnvelopeWindow envelopeWindowOfContext( SRenderContext &ctx )
{
    const QRect r = ctx.getVisibRect();
    SEnvelopeWindow win;
    // Time span of the visible rect: the context maps pixel -> time. The right
    // boundary is open-ended, consistent with STrackRendererInline.
    win.leftTime  = ctx.getTimeOf( r.topLeft().x() );
    win.rightTime = ctx.getTimeOf( r.right() + 1 );
    win.width     = r.width() < 1 ? 1 : r.width();
    return win;
}

bool collectObjectEnvelope( SObject &obj, SLink &lk, const SEnvelopeWindow &win,
                            preview_t *out )
{
    if( !out ) return false;

    offset_t o1 = win.leftTime;
    offset_t o2 = win.rightTime;
    if( o2 <= o1 ) o2 = o1 + 1;
    // Subtract the link start so 0 is the object's own origin.
    o1 -= lk.getStartTime();
    o2 -= lk.getStartTime();

    int w = win.width;
    if( w < 1 ) w = 1;

    return obj.getPreview( out, o1, o2 - o1, w ) >= 0;
}

bool drawObjectWaveform( SObject &obj, SLink &lk, SRenderContext &ctx,
                         const QColor &waveColor )
{
    QPainter &p = ctx.getPainter();
    QRect r = ctx.getVisibRect();
    int tl     = r.topLeft().x();
    int top    = r.topLeft().y();
    int height = r.height();

    // ONE probe-producing path, and it is the collect (proposal 39 D1): a
    // second copy of the pixel->time fold here is how a drawn waveform and a
    // read one come to disagree about the same clip.
    const SEnvelopeWindow win = envelopeWindowOfContext( ctx );
    const int w = win.width;

    QVarLengthArray<preview_t> pv( w );
    if( !collectObjectEnvelope( obj, lk, win, pv.data() ) ) return false;

    // Apply track volume scaling: convert dB to linear gain
    // Snapshot the volume to avoid race conditions with the audio thread.
    // Track parent is typically an STrack; get its volume in dB.
    double volumeGain = 1.0;
    if( SObject *parentObj = dynamic_cast<SObject *>( lk.parent() ) ) {
        // Thread-safe volume read (volumeDbSnapshot holds the volume mutex;
        // the paint path races the UI slider / audio thread).
        volumeGain = pow( 10.0, parentObj->volumeDbSnapshot() / 20.0 );
    }

    p.setPen( waveColor );
    for( int i = 0; i < w; i++ ) {
        int x = i + tl;
        // Scale the preview peaks by the volume gain, clamp to [-127, 127]
        int scaledMin = (int)( pv[i].min * volumeGain );
        int scaledMax = (int)( pv[i].max * volumeGain );
        scaledMin = qBound( -127, scaledMin, 127 );
        scaledMax = qBound( -127, scaledMax, 127 );

        int y1 = top + ( ( 127 - scaledMin ) * height ) / 256;
        int y2 = top + ( ( 127 - scaledMax ) * height ) / 256;
        p.drawLine( x, y1, x, y2 );
    }
    return true;
}
