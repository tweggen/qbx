
#ifndef _STRACK_RNDR_INLINE_H
#define _STRACK_RNDR_INLINE_H

#include <array>

#include <qobject.h>
#include <QColor>
#include <QRgb>
#include "app/model/sobjectrenderer.h"

class STrack;

class STrackRendererInline
    : public SObjectRenderer
{
    Q_OBJECT
public:
    STrackRendererInline( STrack & );
    ~STrackRendererInline();

    virtual void draw( SLink &, SRenderContext & );
    STrack &getTrack() const { return (STrack &)getObject(); }

    /**
     * The colour draw() fills the lane with: the base colour for the selection
     * state, through STrackColorModifier (muted / solo / armed).
     *
     * Public because the M3.10 pixel gate has to know what the FILL is before
     * it can say the overlay is strictly lighter than it, and reading it off
     * the image would mean guessing which colour in a band of pixels is the
     * background - exactly the assumption the gate exists to avoid.
     */
    static QColor laneFillColor( STrack &track );

    /**
     * The Feel Flow compliance band's colour law (proposal 40 M2 follow-up,
     * 2026-08-21): a quantized 24-step hue ramp, LOW compliance (index 0) at
     * red (hue 0 deg) through yellow to HIGH compliance (index 23) at green
     * (hue 120 deg), full saturation, value ~0.85, fully OPAQUE (alpha 255).
     * Traffic-light semantics on purpose, at the requester's explicit request
     * for AGGRESSIVE visibility over the earlier partial-alpha luminance tint
     * ("rainbow, in case I miss shades of grey") -- low compliance is the hot
     * spot to edit, listen to, or overdub.
     *
     * ONE authoritative LUT: the painter (drawFeelFlowBand, strackrndrinline.
     * cpp) and the gate (SMainWindow::describeLaneOverlay's band-mode exact-
     * RGB classification) both read THIS array. Never a second copy of the
     * ramp. Computed once (function-local static); no per-column allocation
     * anywhere in the paint path.
     */
    static constexpr int kFeelFlowPaletteSize = 24;
    static const std::array<QRgb, kFeelFlowPaletteSize> &feelFlowPalette();

    /**
     * The compliance -> palette index quantization (0 = lowest compliance /
     * red, kFeelFlowPaletteSize-1 = highest / green). Public for the same
     * reason feelFlowPalette() is: the gate needs to know the mapping to
     * interpret an observed palette index, not just the colours.
     */
    static int feelFlowPaletteIndex( float compliance );

private:
    class InlineRenderContext
        : public SRenderContext {
    public:
        InlineRenderContext( SRenderContext &, QPainter & );
        virtual ~InlineRenderContext();
        
        SRenderContext &getParentRC() const { return parentRC_; }
        
        virtual offset_t getTimeOf( int x ) const;
    private:
        SRenderContext &parentRC_;
    };

};

#endif
