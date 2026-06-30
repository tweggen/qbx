
#ifndef _TW_SAW_
#define _TW_SAW_

#include "twosc.h"

class tw303aEnvironment;
class twSaw
    : public twOsc 
{
private:
    sample_t smplStateErr;
    sample_t smplState;
    sample_t minVal, maxVal, ampl;
    
    int rampDown;
    
    void __init( void );
protected:
    // Phase 3: IOVector-based interface (type-safe, page-backed)
    virtual length_t calcOutputTo( IOVector& dest, idx_t idx ) override;
    
    sample_t *freqBuffer;
    
public:
    void createOutputLatches( void );
    
    virtual void init( void );
    virtual const char *getInputName( idx_t ) const { return "Simple saw frequency input"; }
    virtual const char *getOutputName( idx_t ) const { return "Simple saw output"; }
    twSaw( tw303aEnvironment &env0 );
    twSaw( tw303aEnvironment &env0, sample_t minVal, sample_t maxVal );
};

#endif

