
#ifndef _SGRAINFILE_RNDR_INLINE
#define _SGRAINFILE_RNDR_INLINE

#include <qobject.h>
#include "sobjectrenderer.h"

class SGrainFile;

class SGrainFileRendererInline
   : public SObjectRenderer
{
public:
    SGrainFileRendererInline( SGrainFile &w ) : SObjectRenderer( (SObject &) w ) {}
    ~SGrainFileRendererInline() {};

    virtual void draw( SLink &, SRenderContext & );
    SGrainFile &getGrainFile() const { return (SGrainFile &) getObject(); }
};

#endif
