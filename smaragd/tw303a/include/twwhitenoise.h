
#ifndef _TW_WHITE_NOISE_
#define _TW_WHITE_NOISE_

#include "twosc.h"

class twWhiteNoise
    : public twOsc
{
    virtual void reset() override;
private:
protected:
    // Phase 3: New IOVector-based interface (type-safe, page-backed)
    virtual length_t calcOutputTo( IOVector& dest, idx_t idx ) override;

    // Legacy raw-pointer interface
    virtual length_t calcOutputTo( sample_t *pDest, length_t length, idx_t idx ) override;
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
