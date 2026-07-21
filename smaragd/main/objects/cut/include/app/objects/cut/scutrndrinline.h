
#ifndef _SCUT_RNDR_INLINE_H
#define _SCUT_RNDR_INLINE_H

#include <qobject.h>
#include <QRect>
#include "app/model/sobjectrenderer.h"

class SCut;

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
    SCut &getCut() const { return (SCut &)getObject(); }

private:
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
