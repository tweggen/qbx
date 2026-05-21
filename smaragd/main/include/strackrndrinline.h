
#ifndef _STRACK_RNDR_INLINE_H
#define _STRACK_RNDR_INLINE_H

#include <qobject.h>
#include "sobjectrenderer.h"

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
