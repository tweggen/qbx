
#ifndef _SPLAINWAVE_H
#define _SPLAINWAVE_H

#include "sobject.h"
#include "sexternfile.h"

class twComponent;
class twRandomSource;
class twWavInput;

class SObjectRenderer;
class SPlainWaveRendererInline;
class SProjectLoader;

/**
 * A Plainwave object is an audio source.
 * Being one of the extern file objects, it is kept inside the project.
 *
 * Thread affinity: MIXED (not thread-safe)
 * - cpWave_: accessed from UI thread (getPreview) AND audio thread (getRootComponent→calcOutputTo)
 * - fileName_: read from UI thread only
 * - inlineRenderer_: UI thread only
 * - previewData_: accessed from UI thread only
 *
 * RACE CONDITION: cpWave_->file_ (QFile) is accessed from both threads without synchronization.
 * Execution paths:
 *   UI:    paintEvent → draw() → getPreview() → getStraightPreview() → straightCalcPreviewData()
 *   Audio: callback → calcOutputTo() → cpWave_->calcOutputTo() → file_.seek/read()
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
    virtual twRandomSource *getRandomSource();
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

