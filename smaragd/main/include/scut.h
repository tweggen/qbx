
#ifndef _SCUT_H_
#define _SCUT_H_

#include <qobject.h>
#include "sobject.h"
#include "slink.h"

class SProject;
class QWidget;
class twComponent;
class SObjectRenderer;
class SCutRendererInline;
class SProjectLoader;

/**
 * A cut (slice) of content with timing information.
 *
 * Thread affinity: MIXED (not thread-safe)
 * - content_: accessed from UI thread (getDetailEditWidget, rendering) AND audio thread (getRootComponent→calcOutputTo)
 * - startOffset_, loopStart_, cutDuration_: read from both UI and audio threads
 *
 * RACE CONDITION: When content_ points to a SPlainWave, the underlying file handle
 * (twWavInput::file_) is accessed from both threads without synchronization.
 *
 * Execution paths:
 *   UI:    SMVActualView::paintEvent() → draw(SLink) → SPlainWaveRendererInline::draw()
 *   Audio: CoreAudio callback → rendering → getRootComponent()->calcOutputTo()
 */
class SCut
    : public SObject
{
    Q_OBJECT
public:
    SCut( SProject *parentProject, SObject &content );
    SCut( SProject *parentProject, SLink &content );
    virtual ~SCut();

    static SLink *instantiateFromDomElement( SProjectLoader &projectLoader, 
					     QDomElement &element, 
					     SObject *parent );

    virtual twComponent &getRootComponent();
    virtual QWidget *getDetailEditWidget( QWidget *parent );
    virtual QWidget *getInlineEditWidget( QWidget *parent );
    virtual SObjectRenderer *getInlineRenderer();    

    virtual int readPostChildrenAttributes( QDomElement &element );
    
    virtual int seekTo( offset_t );
    SObject &getContent() const { return content_->getSObject(); }
    offset_t getLoopStart() const;
    offset_t getStartOffset() const { return startOffset_; }
    virtual bool hasDuration() const { return true; }
    virtual length_t getDuration() const;
    

public slots:
    virtual void setLoopStart( offset_t );
    virtual void setStartOffset( offset_t );
    virtual void setDuration( length_t );

protected:
    virtual int serializeSelfAttributes( QTextStream &o );
    
private:
    SLink *content_;
    offset_t startOffset_;
    offset_t loopStart_;
    length_t cutDuration_;
    SCutRendererInline *inlineRenderer_;
};

#endif
