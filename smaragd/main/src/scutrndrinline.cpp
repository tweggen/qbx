
#include <stdio.h>
#include <qpainter.h>
#include <qobject.h>

#include "sapplication.h"
#include "slink.h"
#include "scut.h"
#include "scutrndrinline.h"

/**
 * The actual cut renderer function.
 * It simply creates a context and calls the content renderer function.
 */
void SCutRendererInline::draw( SLink &lk, SRenderContext &ctx )
{        
    QPainter &p = ctx.getPainter();
    QRect visibRect = ctx.getVisibRect();

    // Now draw the inner of the object.
    InlineRenderContext myctx( getCut(), ctx, p );
    myctx.setVisibRect( visibRect );
    SObjectRenderer *rndr = getCut().getContent().getInlineRenderer();
    if( !rndr ) {
        p.drawText( visibRect, Qt::AlignCenter, "SCut: No renderer." );
        return;
    }
    // Note, that this is my link but his object!!!
    rndr->draw( lk, myctx );
}

/**
 * Return the absolute time (in samples, for now) of the given x position.
 * We calculate it by adjusting the time given by the parent context 
 * with the cut's start offset.
 */
offset_t SCutRendererInline::InlineRenderContext::getTimeOf( int x ) const
{
    return parentRC_.getTimeOf( x )+cut_.getStartOffset();
}

SCutRendererInline::InlineRenderContext::~InlineRenderContext()
{
}

SCutRendererInline::InlineRenderContext::InlineRenderContext( 
    SCut &cut, SRenderContext &par, QPainter &painter )
    : SRenderContext( painter ),
      parentRC_( par ),
      cut_( cut )
{
}


SCutRendererInline::SCutRendererInline( SCut &cut )
    : SObjectRenderer( (SObject &)cut )
{
}

SCutRendererInline::~SCutRendererInline()
{
}
