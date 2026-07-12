
#include <qobject.h>
#include <qpainter.h>

#include "app/model/sobject.h"
#include "app/model/sobjectrenderer.h"

void SRenderContext::setVisibRect( const QRect &rect )
{
    visibRect_ = rect;
}

SRenderContext::SRenderContext( QPainter &painter )
    : painter_( painter )
{
}

SRenderContext::~SRenderContext()
{
}

SObjectRenderer::SObjectRenderer( SObject &sobject )
    : sobject_( sobject )
{
}

SObjectRenderer::~SObjectRenderer()
{
}

