
#ifndef _SPLAINWAVE_H
#define _SPLAINWAVE_H

#include "sobject.h"
#include "sexternfile.h"

class twComponent;
class twWavInput;

class SObjectRenderer;
class SPlainWaveRendererInline;
class SProjectLoader;

/**
 * A Plainwave object is an audio source.
 * Being one of the extern file objects, it is kept inside the project.
 */
class SPlainWave
    : public SExternFile
{
    Q_OBJECT
public:
    SPlainWave( SProject *project );
    virtual ~SPlainWave();
    
    static SLink *instantiateFromDomElement( SProjectLoader &projectLoader, 
					     QDomElement &element, 
					     SObject *parent );

    virtual twComponent &getRootComponent();
    virtual int setWave( const QString url );
    virtual QString getFileName() const;

    // FIXME: Move this to a factory.
    virtual QWidget *getDetailEditWidget( QWidget *parent );
    virtual QWidget *getInlineEditWidget( QWidget *parent );
    virtual SObjectRenderer *getInlineRenderer();

    virtual bool hasDuration() const;
    virtual length_t getDuration() const;
    
    virtual bool hasPreview() const;
    virtual int getPreview( preview_t *dest, 
			    offset_t start, length_t length, 
			    offset_t nProbes );

protected:
    virtual int serializeSelfAttributes( QTextStream &o );

private:
    twWavInput *cpWave_;
    QString fileName_;
    SPlainWaveRendererInline *inlineRenderer_;
};

#endif

