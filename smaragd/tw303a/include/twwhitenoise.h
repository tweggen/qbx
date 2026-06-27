
#ifndef _TW_WHITE_NOISE_
#define _TW_WHITE_NOISE_

#include "twosc.h"

class twWhiteNoise
    : public twOsc
{
    virtual void reset() override;
private:
protected:
    virtual length_t calcOutputTo( sample_t *pDest, length_t length, idx_t idx );
    sample_t *freqBuffer;
    
public:
    void createOutputLatches( void );

    virtual void init( void );
    virtual const char *getInputName( idx_t ) const { return 0; }
    virtual const char *getOutputName( idx_t ) const { return 0; }
    twWhiteNoise( tw303aEnvironment & );
    
    void setBufferSize( length_t newSize );
};

#endif
