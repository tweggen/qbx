
#ifndef _TWGRAINER_H_
#define _TWGRAINER_H_

#include "twcomponent.h"
#include "twgrainspec.h"

/**
 * Playback a given source component using the defined 
 * grainset.
 */
class twGrainer
    : public twComponent 
{
    Q_OBJECT
public:
    twGrainer( tw303aEnvironment &env );
    virtual ~twGrainer();

    virtual bool isSeekable() const;
    virtual int seekTo( offset_t );
    virtual offset_t tellPos() const;

    virtual length_t calcOutputTo( sample_t *pDest, length_t length, idx_t idx );

    virtual void init();

    void setNChannels( idx_t );

    virtual idx_t getNInputs() const;
    virtual idx_t getNOutputs() const;
    virtual const char *getInputName( idx_t idx ) const;
    virtual const char *getOutputName( idx_t idx ) const;

    virtual void setBufferSize( length_t );

    virtual void createOutputLatches();    
    
    length_t getDuration() const;
    twGrainSpec *getGrainSpec() const;
    bool isLooped() const;
    double getStetchFactor() const;
    double getPitchOffset() const;

public slots:
    void setSourceComponent( twComponent *comp );

    void setGrainSpec( twGrainSpec *grainSpec );
    
    /**
     * If set looped is true, the grainer object will
     * continue at its start after the end of the specified
     * range was reached.
     */
    void setLooped( bool );

    /**
     * Stretch factor.
     */
    void setStretchFactor( double );

    /**
     * Pitch offset. Given in cents.
     */
    void setPitchOffset( double );

signals:
    void grainSpecChanged( twGrainSpec *grainSpec );

protected:
    virtual int doInitOperation( int );

private:
    int findValidGrain( /* twGrainSpec *, */ offset_t, int, offset_t * );

    bool isLooped_;
    twGrainSpec *grainSpec_;
    length_t myDuration_;
    int nChannels_;
    offset_t pos_;
    double stretchFactor_;
    double pitchOffset_;
    twComponent *sourceComponent_;
};
#endif
