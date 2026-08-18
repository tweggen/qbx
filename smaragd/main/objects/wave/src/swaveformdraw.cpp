#include "app/objects/wave/swaveformdraw.h"

#include <qpainter.h>
#include <QVarLengthArray>

#include "app/model/sobject.h"
#include "app/model/slink.h"
#include "app/model/sobjectrenderer.h"

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

    // NOTHING SCALES THE PROBES HERE (proposal 39 M2). A drawn waveform
    // describes the audio its object PRODUCES; the lane it is drawn on never
    // scales it. This used to read the containing track's fader
    // (dynamic_cast<SObject*> on lk.parent(), an STrack in every path that
    // creates a clip link) and multiply every probe by it, which was wrong
    // three ways: a waveform is the content and a fader is the level — we have
    // a fader widget AND a level meter for the level; the multiply happened
    // AFTER the probes were quantised to 8 bits, so at -20 dB the envelope
    // collapsed onto a dozen values and drew as a coarse ladder rather than a
    // quieter version of itself; and it contradicted what the stored preview
    // bytes mean (volume is not baked into them — plan/STATE.md:6576-6580), so
    // the paint-time multiply was the entire dependency. At -40 dB the
    // arrangement visually emptied.
    //
    // Gate: preview_envelope_test section 5 (the painted pixels against the
    // collected probes, through a link whose parent holds a non-unity fader).
    //
    // The clamp stays. The probes are already in range, so it costs nothing —
    // and the folder-sum overlay (M3) sums envelopes into this same [-127,127]
    // domain, where a caller handing over a saturated column is the normal case
    // rather than the exotic one.
    p.setPen( waveColor );
    for( int i = 0; i < w; i++ ) {
        int x = i + tl;
        int mn = qBound( -127, (int) pv[i].min, 127 );
        int mx = qBound( -127, (int) pv[i].max, 127 );

        int y1 = top + ( ( 127 - mn ) * height ) / 256;
        int y2 = top + ( ( 127 - mx ) * height ) / 256;
        p.drawLine( x, y1, x, y2 );
    }
    return true;
}
