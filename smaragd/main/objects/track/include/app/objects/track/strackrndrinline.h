
#ifndef _STRACK_RNDR_INLINE_H
#define _STRACK_RNDR_INLINE_H

#include <array>

#include <qobject.h>
#include <QColor>
#include <QFontMetrics>
#include <QRgb>
#include <QString>
#include "app/model/sobjectrenderer.h"

class STrack;
class SObject;

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

    /**
     * THE TAG CHIP (proposal 41 D12-D14, M6). One solid opaque chip per
     * clip, bottom-left corner — the corner D11's start-time-ascending paint
     * order guarantees visible (a clip's left edge can only be covered by a
     * clip that starts strictly to its right), except at an exact start-
     * time tie, where the tiebreak above (child index) makes WHICH tag wins
     * deterministic rather than flaky. It REPLACES the old bottom-right
     * container-asset label SCutRendererInline used to draw — one
     * mechanism now, never a second label.
     *
     * The three statics below are the SHARED decision between draw() (in
     * the .cpp) and the pixel gate (preview_envelope_test.cpp section 7):
     * the gate must read the SAME colour/text/cap logic the paint path
     * used, never a second guess at it (the laneFillColor()/
     * feelFlowPalette() precedent above).
     */

    /**
     * The chip's fill colour (D13): DERIVED from the lane's own final
     * colour (laneFillColor(), post-selection/post-STrackColorModifier),
     * never a fourth hardcoded constant, so it follows track selection and
     * every muted/solo/armed state for free — exactly as the folder-sum
     * overlay's colour does (proposal 39 M3). The RELATION is contractual,
     * not the exact hue: strictly DARKER (lower qGray() luminance) than the
     * lane fill it derives from — darker() scales every channel down
     * identically, so a lower luminance holds for every reachable
     * STrackColorModifier state — and, because the fixed clip-body grey
     * QColor(160,160,160) sits well above every reachable fill luminance
     * (measured: base fill ~64-117, so this chip ~40-73), comfortably
     * darker than the clip body too, which is what makes white chip text
     * readable against it.
     */
    static QColor tagChipColor( const QColor &laneFill );

    /**
     * The tag's DENSITY LADDER (D12/D14): the full name, capped at
     * kTagMaxChars, drawn in full if it fits `availTextWidthPx`; elided
     * (Qt::ElideRight) if a shorter form still fits; or "" — no room for
     * even one character plus an ellipsis, so the caller falls back to a
     * chip with no text at all (the "chip only" rung; "nothing" is the
     * caller's own decision when even the chip does not fit). A PURE
     * function of the name, the font metrics and the pixel budget, so the
     * widths a gate re-derives are the SAME decision draw() made.
     *
     * `*cut`, when non-null, is set true whenever the returned text is not
     * `fullName` itself — the 12-character cap counts as a cut even when
     * the capped text still fits with room to spare, because "the cap is
     * announced" (D14) means the TRUE full name, not the already-capped
     * one: a tooltip is owed whenever what is drawn does not equal the
     * true full name.
     */
    static constexpr int kTagMaxChars = 12;
    static QString tagDensityText( const QString &fullName,
                                   const QFontMetrics &fm,
                                   int availTextWidthPx, bool *cut = nullptr );

    /**
     * The tag's text SOURCE (D12): the fragment/asset name for a
     * container-backed clip (an asset windowing a fragment, a track or the
     * mixer — SCutRendererInline's old bottom-right label read exactly
     * this, `cut.getSName()`), the FILE name for a plain sample-backed
     * clip. Resolved through the type-agnostic SClipWindow/SExternFile
     * model interfaces rather than SCut/SMidiCut/SLaneFragment directly,
     * because `objects/track` may not depend on `objects/cut`,
     * `objects/midi` or `objects/fragment` (tools/check_layering.py) — the
     * same reason the clip loop in the .cpp never dynamic_casts to SCut.
     */
    static QString tagFullText( SObject &clipObject );

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
