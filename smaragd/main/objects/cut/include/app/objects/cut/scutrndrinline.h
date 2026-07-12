
#ifndef _SCUT_RNDR_INLINE_H
#define _SCUT_RNDR_INLINE_H

#include <qobject.h>
#include "app/model/sobjectrenderer.h"

class SCut;

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
