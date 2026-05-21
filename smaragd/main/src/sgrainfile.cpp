
#include <stdio.h>
#include <stdlib.h>

#include <qobject.h>
#include <qwidget.h>
#include <qlabel.h>
#include "qmessagebox.h"

#include "sapplication.h"
#include "sproject.h"
#include "twcomponent.h"
#include "twgrainer.h"
#include "twgrainspec.h"
#include "slink.h"
#include "sgrainfile.h"
#include "sgrainfilerndrinline.h"

bool SGrainFile::hasPreview() const
{
    return true;
}

int SGrainFile::getPreview( preview_t *dest,
                             offset_t start, length_t length,
                             offset_t nProbes )
{
    // Only create preview, if have the wave loaded.
    return getStraightPreview( dest, start, length, nProbes );
}

twComponent &SGrainFile::getRootComponent()
{
    return *(twComponent *)cpGrainer_;
}

QString SGrainFile::getFileName() const
{
    return QString( "Unnamed grain file" );
}

QWidget *SGrainFile::getDetailEditWidget( QWidget */*parent*/ )
{
    return NULL;
}

QWidget *SGrainFile::getInlineEditWidget( QWidget */*parent*/ )
{
    return NULL;
}

SObjectRenderer *SGrainFile::getInlineRenderer()
{
    if( !inlineRenderer_ ) {
        inlineRenderer_ = new SGrainFileRendererInline( *this );        
    }
    return inlineRenderer_;
}

bool SGrainFile::hasDuration() const
{
    return true;
}


length_t SGrainFile::getDuration() const
{
    return cpGrainer_->getDuration();
}

#define GRAIN_LENGTH 1024
#define GRAIN_OVERLAP_IN 128
#define GRAIN_OVERLAP_OUT 128
#define GRAIN_OVERLAP_CENTER (GRAIN_LENGTH-(GRAIN_OVERLAP_OUT+GRAIN_OVERLAP_IN))

twGrainSpec *SGrainFile::createDefaultGrainSpec()
{
    // We assume here, the source object has a duration.
    length_t len = sourceObject_->getSObject().getDuration();
    twGrainSpec *gs;
    twSingleGrainSpec *sgs;

    if( len<GRAIN_LENGTH ) {
        // FIXME: Write this part.
        qWarning( "NYI: create a default grain spec for length < GRAIN_LENGTH" );
        return NULL;
    } 
    length_t nGrains = len-(GRAIN_OVERLAP_IN+GRAIN_OVERLAP_OUT);
    nGrains = (nGrains+GRAIN_OVERLAP_CENTER/2) / GRAIN_OVERLAP_CENTER;
    sgs = (twSingleGrainSpec *) ::calloc( sizeof( twSingleGrainSpec ), nGrains );
    // Evenly distribute grains over time.
    for( int i=0; i<nGrains; i++ ) {
        twSingleGrainSpec *c = sgs+i;
        c->startOffset_ = (offset_t) (GRAIN_OVERLAP_CENTER*i);
        c->length_ = GRAIN_LENGTH;
        c->pitchOffset_ = 0;
        c->overlapIn_ = GRAIN_OVERLAP_IN;
        c->overlapOut_ = GRAIN_OVERLAP_OUT;
    }
    length_t lastLength = len-(length_t)((nGrains-1)*GRAIN_OVERLAP_CENTER);
    sgs[nGrains-1].length_= lastLength;
    gs = new twGrainSpec( *cpGrainer_, sgs, nGrains );
    gs->init();
    return gs;
}

SGrainFile::~SGrainFile()
{
    // FIXME: What do we want to destroy? At least the cpGrainer I believe.
    // But we need to ensure it is no mere connected.
    qWarning( "SGrainFile: dtor: Did not delete any sub-components.\n" );
}

SGrainFile::SGrainFile( SProject *project, SLink *sourceObject )
    : SExternFile( project ),
      sourceObject_( sourceObject )
{
    // FIXME: create grain spec here.
    grainSpec_ = createDefaultGrainSpec();
    cpGrainer_ = new twGrainer( *(SApplication::app().get303aEnvironment()) );
    cpGrainer_->init();
    cpGrainer_->setGrainSpec( grainSpec_ );
}
