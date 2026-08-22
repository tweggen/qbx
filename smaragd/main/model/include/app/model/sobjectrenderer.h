
#ifndef _SOBJECT_RENDERER_H
#define _SOBJECT_RENDERER_H

#include <QRect>
#include <QPainter>
#include <QVarLengthArray>
#include <qobject.h>
#include "tw/graph/tw303aenv.h"

class SObject;
class SLink;

class SRenderContext
{
public:
    SRenderContext( QPainter & );
    virtual ~SRenderContext();

    QPainter &getPainter() const { return painter_; }
    const QRect &getVisibRect() const { return visibRect_; }

    void setVisibRect( const QRect & );

    /**
     * Return the absolute time position of the given visible x coordinate.
     * This of course implies, that x is the time dimension.
     */
    virtual offset_t getTimeOf( int x ) const = 0;

protected:
private:
    QPainter &painter_;
    QRect visibRect_;
};


/**
 * The time window ONE ENVELOPE COLLECT covers: `width` probe columns spanning
 * the half-open TIMELINE range [leftTime, rightTime) (proposal 39 M1, D1).
 *
 * IT CARRIES NO QPainter, DELIBERATELY. An SRenderContext holds a QPainter&
 * and a pixel rect, so a headless caller — the `assert-envelope` verb, and the
 * folder-sum walk M3 builds on this — could only construct one over a scratch
 * QImage: a painter that exists solely to be ignored, which is exactly the sort
 * of prop that later hides a bug in the thing it was propping up. Expressing
 * the collect on a TIME WINDOW instead means the only code that needs a painter
 * is the code that paints.
 *
 * The two times are THE SAME TWO PROBES the draw path takes from its context —
 * ctx.getTimeOf( rect.left() ) and ctx.getTimeOf( rect.right() + 1 ) — and the
 * columns between them are linear, which is what SObject::getPreview() already
 * assumes. drawObjectWaveform() derives this window from its context and then
 * calls the collect, so there is exactly ONE probe-producing path.
 *
 * It lives HERE rather than beside drawObjectWaveform() because
 * SObjectRenderer::collectEnvelope() is declared in this header and app/model
 * may not include app/objects/wave.
 */
struct SEnvelopeWindow
{
    offset_t leftTime  = 0;   // timeline time at column 0's left edge
    offset_t rightTime = 0;   // timeline time PAST the last column
    int      width     = 1;   // number of probe columns to produce
};

/**
 * The window a PAINT path collects over: the two probes drawObjectWaveform()
 * has always taken from its context — ctx.getTimeOf( rect.left() ) and
 * ctx.getTimeOf( rect.right() + 1 ) — and the rect's width.
 *
 * It lives beside SEnvelopeWindow rather than beside drawObjectWaveform()
 * because more than one module has to spell it and they are not allowed to see
 * each other: app/objects/wave derives it for a clip, app/objects/track (the
 * folder-sum overlay, proposal 39 M3) derives it for a lane, and objects/track
 * may not include objects/wave. One spelling, so a lane's overlay and the clips
 * on it cover exactly the same time span.
 */
inline SEnvelopeWindow envelopeWindowOfContext( SRenderContext &ctx )
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

/**
 * Fill `bodyRect` with `bodyColor` only in the COLUMNS where `collect` reports
 * MATERIAL — `min != 0 || max != 0`, the same "silence draws nothing" proxy the
 * folder-sum overlay uses (proposal 41 D10, proposal 39 M3). A gap column is
 * left untouched, so whatever was drawn beneath it — an earlier clip, the lane
 * background, the folder-sum overlay — shows through. No transparency anywhere:
 * this is clipping, not alpha.
 *
 * Returns FALSE and paints NOTHING when `collect` has no envelope to give. The
 * caller then does the ORIGINAL solid fill, because "unknown material" must
 * never read as "no material" — an event clip has no waveform and its window is
 * still a window.
 *
 * ONE spelling, shared by the composite lane's clip loop
 * (STrackRendererInline::draw) and the take lanes (SMVActualView::drawTakeLane).
 * A second copy is exactly how the same grey came to mean "material" on one lane
 * and "window" on the lane directly below it.
 *
 * `collect` is any callable `bool( const SEnvelopeWindow &, preview_t * )`, so
 * the caller decides WHICH envelope — the clip's, or one take's.
 */
template <typename CollectFn>
bool fillBodyByMaterial( QPainter &p, const QRect &bodyRect,
                         const QColor &bodyColor, SRenderContext &ctx,
                         CollectFn &&collect )
{
    const int width = bodyRect.width();
    if( width < 1 ) return false;

    SEnvelopeWindow win;
    win.leftTime  = ctx.getTimeOf( bodyRect.x() );
    win.rightTime = ctx.getTimeOf( bodyRect.x() + width );
    win.width     = width;

    QVarLengthArray<preview_t> pv( width );
    if( !collect( win, pv.data() ) ) return false;

    p.setPen( bodyColor );
    const int top    = bodyRect.y();
    const int bottom = bodyRect.y() + bodyRect.height() - 1;
    for( int i = 0; i < width; ++i ) {
        if( !pv[i].min && !pv[i].max ) continue;   // gap: leave it
        const int x = bodyRect.x() + i;
        p.drawLine( x, top, x, bottom );
    }
    return true;
}

class SObjectRenderer
    : public QObject
{
    Q_OBJECT
public:
    SObjectRenderer( SObject & );
    ~SObjectRenderer();

    virtual void draw( SLink &, SRenderContext & ) = 0;

    /**
     * Fill out[0..win.width) with the envelope this renderer would DRAW over
     * `win`, and return true. The default is `false` and writes NOTHING — an
     * object with no waveform (an event clip) contributes nothing, which is the
     * right answer rather than a bug.
     *
     * It is the same walk `draw()` makes — the same domain mapping, the same
     * loop tiling, the same delegation — with a collect terminal instead of a
     * draw terminal, so a caller never has to know WHAT a clip is
     * (main/timeline/CONTRACT.md inv. 2 forbids the canvas branching on
     * concrete object types) and a second implementation of the mapping cannot
     * drift from the drawn one.
     *
     * The probes are the object's OWN audio, in the preview's signed
     * [-128,127] envelope units. No lane gain is applied here; who scales a
     * contribution is the caller's decision.
     */
    virtual bool collectEnvelope( SLink &, const SEnvelopeWindow &, preview_t * )
    { return false; }

    /**
     * draw() / collectEnvelope() for ONE TAKE of a take COLUMN, rather than for
     * whichever take is audible.
     *
     * `takeIndex < 0` means "the audible one", so the default forwards and every
     * renderer that is not a take column — or is asked for the audible take —
     * is byte-for-byte what it was.
     *
     * It exists so a TAKE LANE is painted by the SAME walk the composite lane
     * uses: same domain map, same loop tiling, same delegation, only the
     * terminal swapped. Before it, SMVActualView::drawTakeLane reached past the
     * clip's own renderer to the take OBJECT and drew it against a bare view
     * context, so every window parameter of a WRAPPING cut (slip, stretch, loop
     * tiling) was silently dropped: on the `SLink -> SCut -> STakeStack` shape
     * the take lane disagreed with what plays by the wrapper's slip, measured at
     * one whole bar on a real project. A take lane is the COMPING surface, so a
     * waveform that does not line up with the audio is not a cosmetic defect.
     */
    virtual void drawTake( SLink &lk, SRenderContext &ctx, int takeIndex )
    { (void) takeIndex; draw( lk, ctx ); }

    virtual bool collectTakeEnvelope( SLink &lk, const SEnvelopeWindow &win,
                                      preview_t *out, int takeIndex )
    { (void) takeIndex; return collectEnvelope( lk, win, out ); }

    SObject &getObject() const { return sobject_; }
    
protected:
    SObject &sobject_;
private:
};

#endif

