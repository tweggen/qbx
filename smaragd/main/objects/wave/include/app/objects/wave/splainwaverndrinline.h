#ifndef _SPLAINWAVE_RNDR_INLINE
#define _SPLAINWAVE_RNDR_INLINE

#include <qobject.h>
#include "app/model/sobjectrenderer.h"

class SPlainWave;

class SPlainWaveRendererInline 
    : public SObjectRenderer
{
    Q_OBJECT
public:
    SPlainWaveRendererInline( SPlainWave &w ) :SObjectRenderer( (SObject &)w ) {};
    ~SPlainWaveRendererInline() {};

    virtual void draw( SLink &, SRenderContext & );
    SPlainWave &getPlainWave() const { return (SPlainWave &)getObject(); }
};

#endif
