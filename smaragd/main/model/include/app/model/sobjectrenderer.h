
#ifndef _SOBJECT_RENDERER_H
#define _SOBJECT_RENDERER_H

#include <QRect>
#include <QPainter>
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

    SObject &getObject() const { return sobject_; }
    
protected:
    SObject &sobject_;
private:
};

#endif

