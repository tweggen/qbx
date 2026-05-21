#ifndef _SGRAINFILE_H
#define _SGRAINFILE_H

#include "sobject.h"
#include "sexternfile.h"

class twComponent;
class twGrainer;
class twGrainSpec;
class SGrainFileRendererInline;
class SObjectRenderer;

/**
 * A grainfile is a collection of specifications to grain a certain object.
 */
class SGrainFile
    : public SExternFile
{
    Q_OBJECT
public:
    SGrainFile( SProject *project, SLink *sourceObject  );
//    SGrainFile( SProject *project, SObject &sourceObject  );
    virtual ~SGrainFile();

    virtual twComponent &getRootComponent();
    virtual QString getFileName() const;
    
    virtual QWidget *getDetailEditWidget( QWidget *parent );
    virtual QWidget *getInlineEditWidget( QWidget *parent );
    virtual SObjectRenderer *getInlineRenderer();

    virtual bool hasDuration() const;
    virtual length_t getDuration() const;

    virtual bool hasPreview() const;
    virtual int getPreview( preview_t *dest, 
                    offset_t start, length_t length, 
                    offset_t nProbes );


private:
    twGrainSpec *createDefaultGrainSpec();

    twGrainer *cpGrainer_;
    twGrainSpec *grainSpec_;
    SLink *sourceObject_;
    SGrainFileRendererInline *inlineRenderer_;
};

#endif
