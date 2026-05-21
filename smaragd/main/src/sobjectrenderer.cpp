
#include <qobject.h>
#include <qpainter.h>

#include "sobject.h"
#include "sobjectrenderer.h"

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

