
#ifndef _STRACK_RNDR_INLINE_H
#define _STRACK_RNDR_INLINE_H

#include <qobject.h>
#include <QColor>
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
