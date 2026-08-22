
#ifndef _SCUT_RNDR_INLINE_H
#define _SCUT_RNDR_INLINE_H

#include <qobject.h>
#include <QRect>
#include <functional>
#include "app/model/sobjectrenderer.h"

class SCut;
class SObject;

// Loop-marker grab handle: the small box drawn at the TOP of every loop boundary
// divider of a looping clip, one text line high — the same visual weight as the
// reference-count numbers the track renderer prints in a clip's upper right.
// Dragging it re-tiles the loop (see SMVActualView::loopMarkerAt).
#define SCUT_LOOP_HANDLE_W 9

// The handle's rect for a boundary at pixel `x` inside `clipRect` (the clip's
// paint rect, i.e. what SCutRendererInline::draw receives as its visible rect).
// SHARED between the renderer that draws it and the arranger that hit-tests it,
// so the two can never drift apart. Returns a null rect when the lane is too
// short to show a grip — no handle is drawn and none can be grabbed.
//
// The height is derived from a fixed small font INSIDE this helper rather than
// from any painter's current font: the arranger leaves the 7pt ruler font on the
// painter after drawing the time ruler, so an ambient-font handle would be drawn
// at one size and hit-tested at another.
QRect scutLoopHandleRect( const QRect &clipRect, int x );

class SCutRendererInline
    : public SObjectRenderer 
{
    Q_OBJECT
public:
    SCutRendererInline( SCut & );
    ~SCutRendererInline();

    virtual void draw( SLink &, SRenderContext & );

    /**
     * The COLLECT terminal of the same walk draw() makes (proposal 39 M1, D2).
     *
     * It is the interesting one: a cut owns the clip->source map (slip offset,
     * stretch, warp anchors), the container/sample branch AND the loop tiling,
     * and every one of those must come out of the collect exactly as it comes
     * out of the paint. Each loop repetition therefore fills ITS OWN pixel span
     * of `out` - returning tile 0's probes for every tile is the failure this
     * shape exists to make impossible, and preview_envelope_test gates it.
     */
    bool collectEnvelope( SLink &, const SEnvelopeWindow &, preview_t * ) override;

    /**
     * The TAKE-LANE terminals of the very same walk. When this cut WRAPS a take
     * column (`SLink -> SCut -> STakeStack`, the shape a shared/placed column
     * has), they draw/collect the NAMED take through this cut's window — its
     * slip, its stretch, its loop tiling — instead of the capture of whichever
     * take is audible. When the content is not a take column they fall back to
     * draw()/collectEnvelope() unchanged.
     *
     * This is what makes a take lane show the take AT THE POSITION IT PLAYS.
     * The wrapper's own badges (pitch, warp handles, gain envelope) are NOT
     * drawn: they belong to the wrapper, and the take draws its own inside the
     * segment.
     */
    void drawTake( SLink &, SRenderContext &, int takeIndex ) override;
    bool collectTakeEnvelope( SLink &, const SEnvelopeWindow &, preview_t *,
                              int takeIndex ) override;

    SCut &getCut() const { return (SCut &)getObject(); }

private:
    /**
     * ONE WALK, N TERMINALS (proposal 39 M1's discipline, extended to the take
     * lanes by this change). The cut's clip->source map and its loop tiling are
     * spelled ONCE each — here for the paint, in collectWalk() for the collect —
     * and every terminal is handed one linear repetition already mapped into the
     * content's own domain. A second copy of either is how a drawn waveform and
     * a read one come to describe different audio.
     */
    void walkSegments( SLink &, SRenderContext &,
                       const std::function<bool( SRenderContext & )> &seg );
    bool collectWalk( SLink &, const SEnvelopeWindow &, preview_t *,
                      const std::function<bool( const SEnvelopeWindow &,
                                                preview_t * )> &seg );
    /** The content's renderer iff the content IS a take column, else null. */
    static SObjectRenderer *takeColumnRendererOf( SObject &content );

    class InlineRenderContext
        : public SRenderContext {
    public:
        InlineRenderContext( SCut &, SRenderContext &, QPainter &, offset_t clipStart );
        virtual ~InlineRenderContext();

        SRenderContext &getParentRC() const { return parentRC_; }
        SCut &getCut() const { return cut_; }
        virtual offset_t getTimeOf( int x ) const;
    private:
        SRenderContext &parentRC_;
        SCut &cut_;
        offset_t clipStart_;   // the clip's link start time (for stretch mapping)
    };

};

#endif
