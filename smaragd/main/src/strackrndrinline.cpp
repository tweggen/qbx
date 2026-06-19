#include <stdio.h>
#include <qpainter.h>
#include <qobject.h>

#include "sapplication.h"
#include "slink.h"
#include "strack.h"
#include "strackrndrinline.h"

/**
 * The actual track renderer function.
 * This one should render first the backings, then ask the contents
 * to render themselves into their off-screens, to then transfer them to screen.
 */
void STrackRendererInline::draw( SLink &, SRenderContext &ctx )
{    
    QPainter &p = ctx.getPainter();
    QRect visibRect = ctx.getVisibRect();

    offset_t leftTime = ctx.getTimeOf( visibRect.x() );
    offset_t rightTime = ctx.getTimeOf( visibRect.x()+visibRect.width() );

    // Draw background with 60% luminosity of the track head colors (c)
    p.fillRect( visibRect, QColor( 77, 77, 115 ) );    
    if( getTrack().isEmpty() ) {
//        p.setPen( QColor( 160, 64, 64 ) );
//        p.drawText( visibRect, AlignCenter, "Track is empty." );
        return;
    }
    // qWarning( "visibRect.x() = %d, leftTime = %d; rightTime=%d.\n", visibRect.x(), (int)leftTime, (int)rightTime );
    for( SLink *lk : getTrack().childLinks() ) {
        // Child tracks are summed into this (folder) track's audio but they are
        // their own lanes — don't draw them as clips here. The lane shows only
        // this track's own clips.
        if( dynamic_cast<STrack*>( &lk->getSObject() ) ) continue;
        bool isSelected = SApplication::app().isSLinkSelected( lk );
        //printf( "Link found: $%08x.\n", lk );
        //fflush( stdout ); fflush( stderr );
        if( !lk->hasStartTime() ) continue;
        if( !lk->getSObject().hasDuration() ) continue;
        offset_t startTime = lk->getStartTime();
//        qWarning( "With start time of %d.\n", (int) startTime );
        length_t length = lk->getSObject().getDuration();
//        qWarning( "And length of %d.\n", (int) length );
        if( startTime >= rightTime || (startTime+length)<leftTime ) continue;
        double relStart = (double)startTime-(double)leftTime;
        double relEnd = (double)(startTime+length)-(double)leftTime;
        double startX = ((double)visibRect.x())
            +( relStart*((double)(visibRect.width()))
               / ((double)(rightTime-leftTime)) );
        if( startX-visibRect.x()>visibRect.width() ) continue;
        double endX = ((double)visibRect.x())
            +( relEnd*((double)(visibRect.width()))
               / ((double)(rightTime-leftTime)) );
        if( endX<visibRect.x() ) continue;
        p.fillRect( (int)startX, visibRect.y(),
                    (int)(endX-startX), visibRect.height(), QColor( 160, 160, 160 ) );
        if( isSelected ) {
            p.setPen( QColor( 255, 255, 255 ) );
            p.drawRect( (int)startX+1, visibRect.y()+1, 
                        (int)(endX-startX)-2, visibRect.height()-2 );
            p.setPen( QColor( 0, 0, 0 ) );
            p.drawRect( (int)startX+2, visibRect.y()+2, 
                        (int)(endX-startX)-4, visibRect.height()-4 );
        }
        // Now draw the inner of the object.
        InlineRenderContext myctx( ctx, p );
        QRect vr( (int)startX+1, visibRect.y()+1, 
                  (int)(endX-startX)-2, visibRect.height()-2 );
        if( vr.topLeft().x()<visibRect.x() ) vr.setLeft( visibRect.x() );
        if( vr.bottomRight().x()>visibRect.bottomRight().x() ) 
            vr.setRight( visibRect.bottomRight().x() );
        if( vr.width()<1 ) continue;
        myctx.setVisibRect( vr );
        //qWarning( "lk is $%08x.\n", (unsigned ) lk );
        SObjectRenderer *rndr = lk->getSObject().getInlineRenderer();
        //qWarning( "rndr is $%08x.\n", (unsigned ) rndr );        
        if( rndr ) {
            rndr->draw( *lk, myctx );
        }
        // Draw the number of links into the upper right.
        {            
            p.setPen( QColor( 0,0,0 ) );
            p.drawText( vr, Qt::AlignTop|Qt::AlignRight, 
                        QString::number( lk->getSObject().getNReferences() ) );
        }
    }
}

/**
 * Return the absolute time (in samples, for now) of the given x position.
 * This depends on the zoom factor of this model.
 */
offset_t STrackRendererInline::InlineRenderContext::getTimeOf( int x ) const
{
    return parentRC_.getTimeOf( x );
}

STrackRendererInline::InlineRenderContext::~InlineRenderContext()
{
}

STrackRendererInline::InlineRenderContext::InlineRenderContext( 
    SRenderContext &par, QPainter &painter )
    : SRenderContext( painter ),
      parentRC_( par )
{    
}


STrackRendererInline::STrackRendererInline( STrack &track )
    : SObjectRenderer( (SObject &)track )
{
}

STrackRendererInline::~STrackRendererInline()
{
}
