
#ifndef _SPLAINWAVE_H
#define _SPLAINWAVE_H

#include "app/model/sobject.h"
#include "app/model/sexternfile.h"

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

    virtual std::shared_ptr<twComponent> getRootComponent();
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

    // Proposal 27 (M0): persist/restore the straight preview via the sidecar
    // store, keyed by the decoded content hash + project rate. UI thread only
    // (called from straightCalcPreviewData) — same affinity previewData_
    // always had, so the cpWave_->file_ race note above is untouched; a
    // sidecar hit actually AVOIDS the racy fallback reads entirely.
    bool fetchPreviewSidecar( preview_t *dest, offset_t nProbes,
                              offset_t skip, offset_t forLength ) override;
    void storePreviewSidecar( const preview_t *data, offset_t nProbes,
                              offset_t skip, offset_t forLength ) override;

private:
    std::shared_ptr<twWavInput> cpWave_;
    QString fileName_;
    SPlainWaveRendererInline *inlineRenderer_;
};

#endif

